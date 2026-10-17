"""parmys-probe: does a Parmys netlist compute what its Verilog source computes? (3C spec 9.2)

Usage::

    parmys-probe [--cycles N] [--warm N] [--mask port=0xH] [--corner] [--prim FILE]
                 SRC.v TOP PARMYS.blif OUTDIR

The BLIF is wrapped as a Verilog module ``blifdut`` (``.names`` become ``assign``s, hard blocks
become instances of VTR's ``vtr_flow/primitives.v`` modules with their ``specify`` blocks
removed, and ``unconn`` is the BLIF's own constant-0 net), a testbench drives the source module
TOP and ``blifdut`` with the same random inputs, and iverilog compares every output.  Inputs
change after each clock edge and outputs are sampled before the next one.  ``--warm N`` writes
the first N cycles (``we`` = 1, the address = cycle + 1) and does not compare them, so a RAM
holds defined words.  ``--mask port=0xH`` ANDs an input with a mask (``addr=0x000e`` holds
``addr[0]`` at 0).  ``--corner`` mixes all-zeros, all-ones and sign corners into wide inputs.
A reference output with an x bit is not compared.

Exit codes: 0 no mismatch; 1 a mismatch (the first is printed as ``cycle N: <port> ref <v> blif
<v>``); 2 usage or tool error; 77 no iverilog on PATH (CTest skip).  Tools run under ``prlimit``
(3 GiB of address space) when it exists.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

EXIT_OK = 0
EXIT_MISMATCH = 1
EXIT_ERROR = 2
EXIT_SKIP = 77

ADDRESS_LIMIT_BYTES = 3 * 1024**3
DEFAULT_CYCLES = 2000
RANDOM_WORD_BITS = 32
MAX_SHOWN_MISMATCHES = 5
WRITE_ENABLES = ("we", "we1")
WRITE_ADDRESSES = ("addr", "wa", "waddr", "addr1")
RAM_MODELS = ("single_port_ram", "dual_port_ram")
OUTPUT_PINS = ("sumout", "cout")
BUS_BIT = re.compile(r"^(.*)~(\d+)$")
PIN_BIT = re.compile(r"^(\w+)\[(\d+)\]$")
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_$]*$")
RESULT = re.compile(r"^RESULT cycles=(\d+) mismatches=(\d+) first=(-?\d+)$")
SHOWN = re.compile(r"^MM (\d+) (\S+) (\S+) (\S+)$")


class ProbeError(Exception):
    """A usage or input error (exit 2)."""


@dataclass
class Blif:
    """The parts of a flat BLIF the wrapper needs."""

    inputs: list[str] = field(default_factory=list)
    outputs: list[str] = field(default_factory=list)
    names: list[tuple[list[str], list[list[str]]]] = field(default_factory=list)
    subckts: list[list[str]] = field(default_factory=list)
    latches: list[list[str]] = field(default_factory=list)


def parse_blif(text: str) -> Blif:
    """Parse the first model of a BLIF (continuation lines joined)."""
    lines = [ln.strip() for ln in text.replace("\\\n", " ").split("\n")]
    blif = Blif()
    i = 0
    while i < len(lines):
        tok = lines[i].split()
        i += 1
        if not tok or tok[0].startswith("#"):
            continue
        if tok[0] == ".end":
            break
        if tok[0] == ".inputs":
            blif.inputs += tok[1:]
        elif tok[0] == ".outputs":
            blif.outputs += tok[1:]
        elif tok[0] == ".names":
            cubes: list[list[str]] = []
            while i < len(lines) and not lines[i].startswith("."):
                if lines[i]:
                    cubes.append(lines[i].split())
                i += 1
            blif.names.append((tok[1:], cubes))
        elif tok[0] == ".subckt":
            blif.subckts.append(tok[1:])
        elif tok[0] == ".latch":
            blif.latches.append(tok[1:])
    return blif


def group_bits(names: Sequence[str]) -> dict[str, dict[int, str] | None]:
    """``x~3`` style bit names grouped into buses: {bus: {bit: name}}, scalars map to None."""
    groups: dict[str, dict[int, str] | None] = {}
    for name in names:
        match = BUS_BIT.match(name)
        if match is None:
            groups[name] = None
            continue
        bus = groups.setdefault(match[1], {})
        if bus is None:
            raise ProbeError(f"port '{match[1]}' is both a scalar and a bus")
        bus[int(match[2])] = name
    return groups


def _decl(width: int | None, name: str) -> str:
    return f"[{width - 1}:0] {name}" if width is not None else name


def _bus_width(bus: dict[int, str] | None) -> int | None:
    return None if bus is None else max(bus) + 1


def _check_ident(groups: dict[str, dict[int, str] | None]) -> None:
    for name in groups:
        if IDENT.match(name) is None:
            raise ProbeError(f"port name '{name}' is not a Verilog identifier")


class _Nets:
    """Stable short Verilog names for BLIF nets."""

    def __init__(self) -> None:
        self.ids: dict[str, str] = {}

    def __call__(self, name: str) -> str:
        if name not in self.ids:
            self.ids[name] = f"n{len(self.ids)}"
        return self.ids[name]


def _names_assign(nets: _Nets, sig: list[str], cubes: list[list[str]]) -> str:
    out, ins = sig[-1], sig[:-1]
    if not ins:
        value = "1'b1" if any(cube[0] == "1" for cube in cubes) else "1'b0"
        return f"assign {nets(out)} = {value};"
    polarity = cubes[0][1] if cubes else "1"
    terms = []
    for cube in cubes:
        lits = [
            nets(x) if ch == "1" else f"~{nets(x)}"
            for ch, x in zip(cube[0], ins, strict=True)
            if ch in "01"
        ]
        terms.append("(" + (" & ".join(lits) or "1'b1") + ")")
    expr = " | ".join(terms) or "1'b0"
    if polarity == "0":
        expr = f"~({expr})"
    return f"assign {nets(out)} = {expr};"


def _is_output_pin(pin: str) -> bool:
    return pin.startswith("out") or pin in OUTPUT_PINS


def _params(model: str, pins: list[tuple[str, str]]) -> str:
    if model in RAM_MODELS:
        div = 2 if model == "dual_port_ram" else 1
        addr = len([p for p, _ in pins if re.match(r"addr\d*\[", p)]) // div
        data = len([p for p, _ in pins if re.match(r"data\d*\[", p)]) // div
        return f"#(.ADDR_WIDTH({addr}), .DATA_WIDTH({max(data, 1)}))"
    if model == "multiply":
        return f"#(.WIDTH({len([p for p, _ in pins if p.startswith('a[')])}))"
    return ""


def _subckt_lines(nets: _Nets, index: int, sub: list[str]) -> list[str]:
    model = sub[0]
    pins = [tuple(p.split("=", 1)) for p in sub[1:]]
    pairs = [(p[0], p[1]) for p in pins if len(p) == 2]
    lines: list[str] = []
    vec: dict[str, dict[int, str]] = {}
    conns: list[str] = []
    for pin, sig in pairs:
        match = PIN_BIT.match(pin)
        if match is None:
            conns.append(f".{pin}({nets(sig)})")
        else:
            vec.setdefault(match[1], {})[int(match[2])] = sig
    for pin, bits in vec.items():
        width = max(bits) + 1
        wire = f"i{index}_{pin}"
        lines.append(f"wire [{width - 1}:0] {wire};")
        for bit in range(width):
            if bit not in bits:
                if not _is_output_pin(pin):
                    lines.append(f"assign {wire}[{bit}] = 1'b0;")
            elif _is_output_pin(pin):
                lines.append(f"assign {nets(bits[bit])} = {wire}[{bit}];")
            else:
                lines.append(f"assign {wire}[{bit}] = {nets(bits[bit])};")
        conns.append(f".{pin}({wire})")
    lines.append(f"{model} {_params(model, pairs)} u{index}({', '.join(conns)});")
    return lines


def _all_signals(blif: Blif) -> set[str]:
    sigs = set(blif.inputs) | set(blif.outputs)
    for sig, _ in blif.names:
        sigs |= set(sig)
    for sub in blif.subckts:
        sigs |= {p.split("=", 1)[1] for p in sub[1:] if "=" in p}
    for latch in blif.latches:
        sigs |= set(latch[:2])
    return sigs


def blif_to_verilog(text: str) -> str:
    """Wrap a BLIF as the Verilog module ``blifdut``."""
    blif = parse_blif(text)
    gin, gout = group_bits(blif.inputs), group_bits(blif.outputs)
    _check_ident(gin)
    _check_ident(gout)
    nets = _Nets()
    out = [f"module blifdut({', '.join([*gin, *gout])});"]
    for name, bus in gin.items():
        out.append("input " + _decl(_bus_width(bus), name) + ";")
    for name, bus in gout.items():
        out.append("output " + _decl(_bus_width(bus), name) + ";")
    out += [f"wire {nets(s)};" for s in sorted(_all_signals(blif))]
    for name, bus in gin.items():
        if bus is None:
            out.append(f"assign {nets(name)} = {name};")
        else:
            out += [f"assign {nets(s)} = {name}[{k}];" for k, s in bus.items()]
    for name, bus in gout.items():
        if bus is None:
            out.append(f"assign {name} = {nets(name)};")
        else:
            out += [f"assign {name}[{k}] = {nets(s)};" for k, s in bus.items()]
    out += [_names_assign(nets, sig, cubes) for sig, cubes in blif.names]
    for k, latch in enumerate(blif.latches):
        clk = latch[3] if len(latch) > 3 else "clk"
        out.append(
            f"reg l{k}; assign {nets(latch[1])} = l{k}; "
            f"always @(posedge {nets(clk)}) l{k} <= {nets(latch[0])};"
        )
    for k, sub in enumerate(blif.subckts):
        out += _subckt_lines(nets, k, sub)
    out.append("endmodule")
    return "\n".join(out) + "\n"


def _random_expr(width: int) -> str:
    words = (width + RANDOM_WORD_BITS - 1) // RANDOM_WORD_BITS
    return "{" + ", ".join(["$random"] * words) + "}"


def _stim_lines(name: str, width: int, mask: str | None, corner: bool) -> list[str]:
    lines = [f"    {name} = {_random_expr(width)};"]
    if mask:
        lines.append(f"    {name} = {name} & {mask.replace('0x', chr(39) + 'h')};")
    if corner and width > 1:
        rest = width - 1
        lines += [
            "    if ($random % 4 == 0) begin rnd = $random;",
            f"      if (rnd[1:0] == 0) {name} = {{{width}{{1'b0}}}};",
            f"      else if (rnd[1:0] == 1) {name} = {{{width}{{1'b1}}}};",
            f"      else if (rnd[1:0] == 2) {name} = {{1'b1, {{{rest}{{1'b0}}}}}};",
            f"      else {name} = {{1'b0, {{{rest}{{1'b1}}}}}};",
        ]
        if mask:
            lines.append(f"      {name} = {name} & {mask.replace('0x', chr(39) + 'h')};")
        lines.append("    end")
    return lines


@dataclass
class TbOptions:
    """What the testbench generator needs."""

    top: str
    cycles: int
    warm: int
    masks: dict[str, str]
    corner: bool


def make_testbench(blif: Blif, opts: TbOptions) -> str:
    """The testbench: source TOP (r_ outputs) against blifdut (d_ outputs)."""
    gin, gout = group_bits(blif.inputs), group_bits(blif.outputs)
    tb = ["`timescale 1ns/1ps", "module tb;"]
    for name, bus in gin.items():
        tb.append("reg " + _decl(_bus_width(bus), name) + ";")
    if "clk" not in gin:
        tb.append("reg clk;")
    for name, bus in gout.items():
        for pre in ("r_", "d_"):
            tb.append("wire " + _decl(_bus_width(bus), pre + name) + ";")
    for inst, mod, pre in (("rf", opts.top, "r_"), ("dut", "blifdut", "d_")):
        conns = [f".{g}({g})" for g in gin] + [f".{g}({pre}{g})" for g in gout]
        tb.append(f"{mod} {inst}({', '.join(conns)});")
    tb.append("integer cyc, bad, first; reg [31:0] rnd;")
    tb.append("initial begin bad = 0; first = -1; clk = 0;")
    tb += [f"  {g} = 0;" for g in gin if g != "clk"]
    tb.append("  stim;")
    tb.append(f"  for (cyc = 0; cyc < {opts.cycles}; cyc = cyc + 1) begin")
    tb.append("    #1 clk = 1; #5 clk = 0; #1; stim;")
    if opts.warm:
        sets = [f"{g} = 1;" for g in WRITE_ENABLES if g in gin]
        sets += [f"{g} = cyc + 1;" for g in WRITE_ADDRESSES if g in gin]
        tb.append(f"    if (cyc < {opts.warm}) begin {' '.join(sets)} end")
    tb.append(f"    #1; if (cyc >= {opts.warm}) sample;")
    tb.append("  end")
    tb.append(
        f'  $display("RESULT cycles=%0d mismatches=%0d first=%0d", {opts.cycles}, bad, first);'
        " $finish; end"
    )
    tb.append("task stim; begin")
    for name, bus in gin.items():
        if name != "clk":
            tb += _stim_lines(name, _bus_width(bus) or 1, opts.masks.get(name), opts.corner)
    tb.append("end endtask")
    tb.append("task sample; begin")
    for name in gout:
        tb.append(
            f"  if ((^r_{name} !== 1'bx) && (d_{name} !== r_{name})) begin bad = bad + 1;"
            f" if (first < 0) first = cyc; if (bad <= {MAX_SHOWN_MISMATCHES})"
            f' $display("MM %0d {name} %h %h", cyc, r_{name}, d_{name}); end'
        )
    tb.append("end endtask")
    tb.append("endmodule")
    return "\n".join(tb) + "\n"


def _limited(cmd: list[str]) -> list[str]:
    if shutil.which("prlimit"):
        return ["prlimit", f"--as={ADDRESS_LIMIT_BYTES}", *cmd]
    return cmd


def _default_prim() -> Path:
    root = os.environ.get("VTR_ROOT")
    base = Path(root) if root else Path.home() / "odin3-ws/external/vtr-verilog-to-routing"
    return base / "vtr_flow" / "primitives.v"


def _strip_specify(text: str) -> str:
    return re.sub(r"^\s*specify\b.*?^\s*endspecify", "", text, flags=re.S | re.M)


def _write_files(args: argparse.Namespace, blif_text: str, outdir: Path) -> list[Path]:
    """Writes blifdut.v, tb.v and the primitives; returns the files to compile."""
    blif = parse_blif(blif_text)
    masks = dict(item.split("=", 1) for item in args.mask)
    opts = TbOptions(args.top, args.cycles, args.warm, masks, args.corner)
    (outdir / "blifdut.v").write_text(blif_to_verilog(blif_text))
    (outdir / "tb.v").write_text(make_testbench(blif, opts))
    files = [outdir / "tb.v", outdir / "blifdut.v", Path(args.src)]
    prim = Path(args.prim) if args.prim else _default_prim()
    if prim.is_file():
        (outdir / "prim_nospec.v").write_text(_strip_specify(prim.read_text()))
        files.append(outdir / "prim_nospec.v")
    elif blif.subckts:
        raise ProbeError(f"hard blocks need VTR's primitives.v; {prim} not found (--prim)")
    return files


def _report(stdout: str) -> int:
    first_shown = True
    result: re.Match[str] | None = None
    for line in stdout.splitlines():
        shown = SHOWN.match(line)
        if shown and first_shown:
            print(f"cycle {shown[1]}: {shown[2]} ref {shown[3]} blif {shown[4]}")
            first_shown = False
        elif shown:
            print(f"  also cycle {shown[1]}: {shown[2]} ref {shown[3]} blif {shown[4]}")
        result = RESULT.match(line) or result
    if result is None:
        raise ProbeError("the simulation printed no RESULT line:\n" + stdout)
    print(f"{result[1]} cycles, {result[2]} mismatches, first at cycle {result[3]}")
    return EXIT_MISMATCH if int(result[2]) else EXIT_OK


def run_probe(args: argparse.Namespace) -> int:
    """Run the probe described by args (the parsed command line); returns the exit code."""
    if shutil.which("iverilog") is None or shutil.which("vvp") is None:
        print("parmys-probe: iverilog not found on PATH", file=sys.stderr)
        return EXIT_SKIP
    try:
        outdir = Path(args.outdir)
        outdir.mkdir(parents=True, exist_ok=True)
        files = _write_files(args, Path(args.blif).read_text(), outdir)
        sim = outdir / "sim.vvp"
        comp = subprocess.run(
            _limited(["iverilog", "-g2012", "-o", str(sim), *map(str, files)]),
            capture_output=True,
            text=True,
            check=False,
        )
        if comp.returncode != 0:
            raise ProbeError("iverilog failed:\n" + comp.stderr)
        run = subprocess.run(
            _limited(["vvp", str(sim)]), capture_output=True, text=True, check=False
        )
        return _report(run.stdout)
    except (ProbeError, OSError) as exc:
        print(f"parmys-probe: {exc}", file=sys.stderr)
        return EXIT_ERROR


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog="parmys-probe", description=__doc__.split("\n")[0])
    ap.add_argument("src")
    ap.add_argument("top")
    ap.add_argument("blif")
    ap.add_argument("outdir")
    ap.add_argument("--cycles", type=int, default=DEFAULT_CYCLES)
    ap.add_argument("--warm", type=int, default=0)
    ap.add_argument("--mask", action="append", default=[], metavar="PORT=0xH")
    ap.add_argument("--corner", action="store_true")
    ap.add_argument("--prim", default="")
    return ap.parse_args(list(argv))


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = parse_args(sys.argv[1:] if argv is None else argv)
    except SystemExit as exc:
        return exc.code if isinstance(exc.code, int) else EXIT_ERROR
    return run_probe(args)


if __name__ == "__main__":
    sys.exit(main())
