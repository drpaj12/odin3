"""quartus-check: one micro through ``write_verilog --quartus``, the purity checks and a
random-simulation equivalence against the RTL.

Usage::

    quartus-check [--odin3 P] [--lib L] [--script S] [--cycles N] [--seed N] [--keep DIR]
                  [--top NAME] [--icells] [--gold RTL.v] MICRO.v

``--icells`` reads the micro with ``read_verilog -icells`` (a micro that instantiates Odin-internal
cells such as ``\\$sdffe``); ``--gold`` names the behavioural RTL to compare with instead of the
micro itself (same module name and ports).

Legs, in order (the first failure is printed and the exit status is 1):

1. ``flow``: Odin III runs the LPM script (``tools/flow/lpm.script``, which ends in
   ``write_verilog --quartus``) and writes the netlist and ``<out>_lpm_sim.v``.
2. ``pure``: the netlist holds no Odin-internal cell type (a ``$``-named type instance), no
   ``module`` definition of a library name (``lpm_*``, ``altsyncram``, ``scfifo``), no
   ``blackbox`` attribute and no ``$_`` name; the sim file holds nothing but stubs.
3. ``readback``: the netlist alone, then the netlist with the sim file, is read back with the LPM
   library (``read_verilog``, ``proc``, ``--check``).
4. ``lint``: ``iverilog -g2005 -Wall`` reports nothing on the netlist plus the sim file (the
   floating-input notes of unconnected LPM defaults aside).
5. ``equiv``: a testbench built from the micro's port list drives both the RTL (renamed ``gold``)
   and the netlist with the same random inputs (the control inputs named ``clk*`` are clocks;
   every other input is random at each falling edge, asynchronous controls included) and compares
   every output with ``!==`` shortly after each edge and each input change. A micro that
   instantiates a library cell (the stubs are empty) skips this leg.

Exit 77 when ``iverilog`` is not installed.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LIB = ROOT / "lib" / "lpm.o3lib"
DEFAULT_SCRIPT = ROOT / "tools" / "flow" / "lpm.script"
TIMEOUT = 120
FLOATING = re.compile(r"dangling input port .* floating")
# An instance of an Odin-internal cell type: `\$type #(` or `\$type name (` at the start of a
# statement (a name such as `\$c12 <= ...` or `\$c12 [a] <= ...` is not one).
INTERNAL_INSTANCE = re.compile(
    r"^\s*(?:\(\*[^\n]*?\*\)\s*)?\\\$\S+\s+(?:#\(|[^<=\[\s]\S*\s*\()", re.M
)
LIB_NAMES = "lpm_add_sub|lpm_mult|lpm_compare|altsyncram|scfifo|dcfifo"
LIBRARY_MODULE = re.compile(rf"^\s*module\s+\\?({LIB_NAMES})\b", re.M)
LIBRARY_INSTANCE = re.compile(rf"^\s*(?:\(\*[^\n]*?\*\)\s*)?({LIB_NAMES})\s+#?\(", re.M)
PORT_RE = re.compile(
    r"^\s*(input|output|inout)?\s*(?:reg|wire)?\s*(\[[^\]]+\])?\s*(\w+)\s*$"
)


class CheckError(Exception):
    """A leg failed (exit 1) or the tool could not run (exit 2, usage=True)."""

    def __init__(self, message: str, usage: bool = False) -> None:
        super().__init__(message)
        self.usage = usage


def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=TIMEOUT, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CheckError(f"cannot run {cmd[0]}: {exc}", usage=True) from exc


def odin3_run(odin3: str, script: str, extra: list[str] | None = None) -> str:
    proc = run([odin3, *(extra or []), "-p", script])
    if proc.returncode != 0:
        tail = "\n".join(proc.stderr.strip().splitlines()[-5:])
        raise CheckError(f"odin3 failed ({proc.returncode}) on: {script}\n{tail}")
    return proc.stderr


def script_text(script: Path, lib: Path, design: Path, out: Path) -> str:
    lines = [ln for ln in script.read_text().splitlines() if ln.strip() and not ln.startswith("#")]
    if len(lines) != 1:
        raise CheckError(f"{script}: expected one script line, found {len(lines)}", usage=True)
    text = lines[0].replace("@LIB@", str(lib)).replace("@DESIGN@", str(design))
    return text.replace("@FAMILY@", "").replace("@OUT@", str(out)).replace("  ", " ")


def sim_file(out: Path) -> Path:
    return out.with_name(out.stem + "_lpm_sim.v")


def check_pure(netlist: str, sim: str) -> None:
    netlist = re.sub(r"//[^\n]*", "", netlist)  # the cell-type comments name `$_DFF_...`
    hit = INTERNAL_INSTANCE.search(netlist)
    if hit:
        raise CheckError(f"pure: an Odin-internal cell instance: {hit.group(0).strip()[:60]}")
    lib = LIBRARY_MODULE.search(netlist)
    if lib:
        raise CheckError(f"pure: the netlist defines the library module {lib.group(1)}")
    for needle in ("(* blackbox", "$_", "gclk"):
        if needle in netlist:
            raise CheckError(f"pure: the netlist holds '{needle}'")
    body = [ln for ln in sim.splitlines() if ln.strip() and not ln.startswith("//")]
    if any(not re.match(r"^(\(\*|module|endmodule|\s|parameter|input|output|inout|\)|#)", ln)
           for ln in body):
        raise CheckError("pure: the sim file holds something besides stubs")


def check_readback(odin3: str, lib: Path, netlist: Path, sim: Path) -> None:
    for files in (str(netlist), f"{netlist} {sim}"):
        odin3_run(odin3, f"read_techlib {lib}; read_verilog {files}; proc; opt", ["--check"])


def check_lint(netlist: Path, sim: Path) -> None:
    proc = run(["iverilog", "-g2005", "-Wall", "-o", "/dev/null", str(netlist), str(sim)])
    notes = [ln for ln in proc.stderr.splitlines() if ln.strip() and not FLOATING.search(ln)]
    if proc.returncode != 0 or notes:
        raise CheckError("lint: iverilog: " + "; ".join(notes[:3] or [f"exit {proc.returncode}"]))


def ports(text: str, top: str) -> list[tuple[str, str, str]]:
    """(direction, range, name) of the ports of module ``top`` (an ANSI header)."""
    match = re.search(rf"module\s+{top}\s*(?:#\([^)]*\))?\s*\((.*?)\)\s*;", text, re.S)
    if match is None:
        raise CheckError(f"equiv: no ANSI header for module {top}", usage=True)
    found: list[tuple[str, str, str]] = []
    direction, width = "", ""
    for chunk in match.group(1).split(","):
        item = re.sub(r"\(\*.*?\*\)|//[^\n]*", "", chunk, flags=re.S)
        head = PORT_RE.match(item.replace("\n", " "))
        if head is None:
            raise CheckError(f"equiv: cannot read the port '{item.strip()}'", usage=True)
        if head.group(1):
            direction, width = head.group(1), head.group(2) or ""
        found.append((direction, width, head.group(3)))
    return found


def testbench(top: str, plist: list[tuple[str, str, str]], cycles: int, seed: int) -> str:
    ins = [(w, n) for d, w, n in plist if d == "input"]
    outs = [(w, n) for d, w, n in plist if d == "output"]
    clocks = [n for _, n in ins if n.startswith("clk")]
    lines = ["`timescale 1ns/1ps", "module tb;"]
    lines += [f"  reg {w} {n};" for w, n in ins]
    lines += [f"  wire {w} g_{n}, d_{n};" for w, n in outs]

    def pins(prefix: str) -> str:
        return ", ".join(
            f".{n}({n})" if d == "input" else f".{n}({prefix}{n})" for d, _, n in plist
        )

    lines.append(f"  gold gold_u ({pins('g_')});")
    lines.append(f"  {top} dut ({pins('d_')});")
    lines.append("  integer tb_i, tb_bad;")
    lines.append("  task tb_cmp; begin")
    for _, n in outs:
        lines.append(f"    if (g_{n} !== d_{n}) begin tb_bad = tb_bad + 1; "
                     f'$display("MISMATCH %0t {n}: gold=%b dut=%b", $time, g_{n}, d_{n}); end')
    lines.append("  end endtask")
    lines.append("  initial begin")
    lines.append(f"    tb_bad = 0; tb_i = {seed};")
    lines += [f"    {n} = 0;" for _, n in ins]
    lines.append(f"    for (tb_i = 0; tb_i < {cycles}; tb_i = tb_i + 1) begin")
    lines += [f"      {n} = 1;" for n in clocks]
    lines += ["      #2; tb_cmp; #3;"]
    lines += [f"      {n} = 0;" for n in clocks]
    lines.append("      #1;")
    for _, n in ins:
        if n not in clocks:
            rate = "(($random & 7) == 0)" if re.search(r"rst|clr|set|ald", n) else "$random"
            lines.append(f"      {n} = {rate};")
    lines += ["      #2; tb_cmp; #2;", "    end"]
    lines.append(
        '    if (tb_bad == 0) $display("EQUIV PASS"); else $display("EQUIV FAIL %0d", tb_bad);'
    )
    lines += ["    $finish;", "  end", "endmodule"]
    return "\n".join(lines) + "\n"


def check_equiv(micro: Path, netlist: Path, sim: Path, top: str, args: argparse.Namespace,
                work: Path) -> str:
    rtl = micro.read_text()
    if LIBRARY_INSTANCE.search(netlist.read_text()) or LIBRARY_INSTANCE.search(rtl):
        return "equiv: skipped (a library cell is instantiated; its stub is empty)"
    plist = ports(rtl, top)
    gold = work / "gold.v"
    gold.write_text(re.sub(rf"\bmodule\s+{top}\b", "module gold", rtl))
    tb = work / "tb.v"
    tb.write_text(testbench(top, plist, args.cycles, args.seed))
    exe = work / "sim.vvp"
    comp = run(["iverilog", "-g2005", "-o", str(exe), str(tb), str(gold), str(netlist), str(sim)])
    if comp.returncode != 0:
        raise CheckError("equiv: iverilog: " + (comp.stderr.strip().splitlines() or ["failed"])[0])
    out = run(["vvp", str(exe)]).stdout
    if "EQUIV PASS" not in out:
        first = [ln for ln in out.splitlines() if "MISMATCH" in ln][:2]
        raise CheckError("equiv: " + "; ".join(first or ["no verdict"]))
    return f"equiv: {args.cycles} random cycles, outputs equal"


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog="quartus-check", description=__doc__.split("\n")[0])
    ap.add_argument("micro", type=Path)
    ap.add_argument("--odin3", default="odin3")
    ap.add_argument("--lib", type=Path, default=DEFAULT_LIB)
    ap.add_argument("--script", type=Path, default=DEFAULT_SCRIPT)
    ap.add_argument("--cycles", type=int, default=400)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--top", default=None)
    ap.add_argument("--icells", action="store_true")
    ap.add_argument("--gold", type=Path, default=None)
    ap.add_argument("--keep", type=Path, default=None)
    return ap.parse_args(argv)


def check_micro(args: argparse.Namespace, work: Path) -> list[str]:
    out = work / "netlist.v"
    text = script_text(args.script, args.lib, args.micro, out)
    if args.icells:
        text = text.replace("read_verilog ", "read_verilog -icells ", 1)
    odin3_run(args.odin3, text)
    sim = sim_file(out)
    if not out.is_file() or not sim.is_file():
        raise CheckError("flow: the netlist or the sim file was not written")
    check_pure(out.read_text(), sim.read_text())
    check_readback(args.odin3, args.lib, out, sim)
    check_lint(out, sim)
    found = re.search(r"module\s+(\w+)", (args.gold or args.micro).read_text())
    top = args.top or (found.group(1) if found else "")
    return [check_equiv(args.gold or args.micro, out, sim, top, args, work)]


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    if shutil.which("iverilog") is None:
        print("quartus-check: iverilog is not installed", file=sys.stderr)
        return 77
    if not args.micro.is_file():
        print(f"quartus-check: no such file: {args.micro}", file=sys.stderr)
        return 2
    try:
        with tempfile.TemporaryDirectory(prefix="quartus-check.") as tmp:
            work = Path(tmp)
            try:
                notes = check_micro(args, work)
            finally:
                if args.keep is not None:
                    shutil.copytree(work, args.keep, dirs_exist_ok=True)
    except CheckError as exc:
        print(f"quartus-check: {args.micro.name}: {exc}", file=sys.stderr)
        return 2 if exc.usage else 1
    print(f"quartus-check: {args.micro.name}: ok ({'; '.join(notes)})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
