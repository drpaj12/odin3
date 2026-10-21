"""netlist-ingest: the Phase 6.1 conformance driver for gate-level netlist ingest (spec
``docs/specs/2026-10-21-6-analysis-flow-design.md`` section 1, G-1 to G-7).

The fixtures are ``tests/netlist/<micro>/{rtl.v, mapped.v, mapped_src.v, mapped.blif}`` and the cell
library ``tests/netlist/cells/cmos.v`` (``tests/netlist/gen.sh`` makes them with Yosys 0.55). No new
reader exists: the Verilog netlist is read by ``read_verilog`` with the library read first
(``--lib``), the BLIF by ``read_blif``. Commands (all take ``--odin3 PATH``; ``equiv`` also
``--equiv-check PATH``):

- ``cells``              G-2: each cell model of ``cmos.v`` elaborates alone (``--top CELL``) and
                         the file is a ``--lib`` read.
- ``equiv MICRO FORM``   G-3, G-5: ``FORM`` is ``verilog`` or ``blif``; the netlist is read,
                         ``proc``, ``flatten``, ``lower``, mapped to LUTs (``abc --lut 6``, because
                         ``equiv-check`` reads no gate cells), written as BLIF, and ``equiv-check``
                         must say equivalent to the same flow on the RTL (``rtl.v``).
- ``names MICRO``        G-3, G-4: default mode (cells stay cells): every cell type used is an IR
                         module; every instance name and wire name of ``mapped.v`` (escaped
                         identifiers included) and every net name of ``mapped.blif`` is in the
                         ``dump_locs`` of the ingested design.
- ``prov MICRO``         G-6: ``dump_locs`` of the Verilog netlist prints a netlist location
                         (``mapped.v:LINE``, kind SOURCE) for the module, its wires and instances;
                         of the BLIF prints none (kind IMPORTED is a netlist line, not an HDL
                         location).
- ``dump MICRO``         ``names`` then ``prov`` (the CTest ``netlist_dump_<micro>``).
- ``src MICRO``          G-6: ``mapped_src.v`` read with ``read_verilog -netlist`` (5.4c) restores
                         the RTL location (``rtl.v``, as the ``src`` attribute spells it).
- ``unknown``            G-7: a cell no library defines is a located ``unresolved_module`` error;
                         with ``--auto-blackbox`` it is an implicit black box whose port
                         directions come from its nets (EL-11).

Every child runs under the memory cap of ``memcap`` (``tools/elab-check``). Exit codes: 0 the check
passed, 1 it failed, 2 usage or tool error, 77 skipped (no cap available).
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

import memcap

ROOT = Path(__file__).resolve().parents[2]
NETLIST = Path("tests/netlist")
CELLS = NETLIST / "cells" / "cmos.v"
CELL_NAMES = ("BUF", "NOT", "NAND2", "NOR2", "DFF", "DFFSR")
FORMS = ("verilog", "blif")
CAP_BYTES = 2 << 30
TIMEOUT_SECONDS = 600
LUT_K = 6
EXIT_PASS, EXIT_FAIL, EXIT_ERROR, EXIT_SKIP = 0, 1, 2, 77

_DECL = re.compile(
    r"^\s*(?:wire|input|output|inout)\b\s*(?:\[[^\]]*\]\s*)?(\\\S+|[A-Za-z_]\w*)\s*;")
_INSTANCE = re.compile(r"^\s*([A-Za-z_]\w*)\s+(\\\S+|[A-Za-z_]\w*)\s*\($")
_MODULE = re.compile(r"^\s*module\s+([A-Za-z_]\w*)")
_SRC_RTL = re.compile(r'\(\*\s*src\s*=\s*"rtl\.v:')
_BLIF_PIN = re.compile(r"^\S+=(\S+)$")


class ToolError(Exception):
    """A tool problem (not a check failure): exit 2."""


def run(argv: Sequence[str]) -> tuple[int, str]:
    """argv under the memory cap, from the repository root: (exit code, stdout plus stderr)."""
    try:
        done = memcap.run(list(argv), CAP_BYTES, capture_output=True, cwd=str(ROOT),
                          timeout=TIMEOUT_SECONDS)
    except OSError as exc:
        raise ToolError(f"cannot run {argv[0]}: {exc}") from exc
    return done.returncode, done.stdout + done.stderr


def odin3(binary: str, script: str) -> tuple[int, str]:
    """One Odin III run of the pass script."""
    return run([binary, "-p", script])


def first_error(text: str) -> str:
    """The first error line of Odin III output (all of it when there is none)."""
    for line in text.splitlines():
        if "error:" in line:
            return line.strip()
    return text.strip()[:200]


def top_of(rtl: Path) -> str:
    """The first module name of a Verilog file."""
    for line in (ROOT / rtl).read_text().splitlines():
        match = _MODULE.match(line)
        if match is not None:
            return match[1]
    raise ToolError(f"{rtl}: no module")


def micro_dir(micro: str) -> Path:
    """The fixture directory of a micro."""
    path = NETLIST / micro
    if not (ROOT / path / "rtl.v").is_file():
        raise ToolError(f"no fixture {path}")
    return path


def read_script(micro: Path, form: str) -> str:
    """The read commands for the netlist of a micro in a form (library first for Verilog)."""
    if form == "blif":
        return f"read_blif {micro / 'mapped.blif'}"
    return f"read_verilog --lib {CELLS}; read_verilog {micro / 'mapped.v'}"


def flat_blif(binary: str, read: str, out: Path) -> str | None:
    """read, proc, flatten, lower, LUT-map, write_blif out; None, else the failure text."""
    script = f"{read}; proc; flatten; lower; check; abc --lut {LUT_K}; write_blif {out}"
    code, text = odin3(binary, script)
    return None if code == 0 else first_error(text)


def check_cells(args: argparse.Namespace) -> int:
    """G-2: each cell model elaborates alone and as a ``--lib`` read."""
    failures = 0
    code, text = odin3(args.odin3, f"read_verilog --lib {CELLS}")
    if code != 0:
        print(f"FAIL cmos.v --lib read: {first_error(text)}")
        failures += 1
    for cell in CELL_NAMES:
        done, text = run([args.odin3, "--top", cell, "-p",
                          f"read_verilog {CELLS}; proc; lower; check"])
        if done != 0:
            print(f"FAIL {cell}: {first_error(text)}")
            failures += 1
    print(f"netlist cells: {len(CELL_NAMES)} models, {failures} failed")
    return EXIT_FAIL if failures else EXIT_PASS


def check_equiv(args: argparse.Namespace) -> int:
    """G-3, G-5: the netlist and the RTL, flattened and lowered, are equivalent."""
    micro = micro_dir(args.micro)
    with tempfile.TemporaryDirectory(prefix="netlist-ingest-") as tmp:
        rtl_out, net_out = Path(tmp, "rtl.blif"), Path(tmp, "net.blif")
        problem = flat_blif(args.odin3, f"read_verilog {micro / 'rtl.v'}", rtl_out)
        if problem is None:
            problem = flat_blif(args.odin3, read_script(micro, args.form), net_out)
        if problem is not None:
            print(f"FAIL {args.micro} {args.form}: {problem}")
            return EXIT_FAIL
        code, text = run([args.equiv_check, str(rtl_out), str(net_out)])
    verdict = [line for line in text.splitlines()
               if line.startswith("equiv-check:") and "notice:" not in line]
    last = verdict[-1] if verdict else text.strip()[:200]
    if code == 0:
        print(f"netlist_ingest {args.micro} {args.form}: {last}")
        return EXIT_PASS
    print(f"FAIL {args.micro} {args.form}: equiv-check exit {code}: {last}")
    return EXIT_FAIL if code == 1 else EXIT_ERROR


def dump_lines(binary: str, read: str) -> tuple[list[tuple[str, str, str, str]], str]:
    """``read; dump_locs`` as (module, kind, name, SRC) rows; the second value is the failure
    text, empty when the run worked."""
    code, text = odin3(binary, f"{read}; dump_locs")
    if code != 0:
        return [], first_error(text)
    rows = []
    for line in text.splitlines():
        fields = line.split("|", 3)  # a location may hold "|" (several, joined)
        if len(fields) == 4 and fields[1] in ("module", "wire", "net", "node"):
            rows.append((fields[0], fields[1], fields[2], fields[3]))
    return rows, ""


def unescape(name: str) -> str:
    """The identifier of a Verilog name: ``\\a.b`` is ``a.b``."""
    return name[1:] if name.startswith("\\") else name


def verilog_names(path: Path) -> tuple[set[str], set[str], set[str]]:
    """(wire and port names, instance names, cell types used) of a Verilog netlist."""
    wires: set[str] = set()
    instances: set[str] = set()
    types: set[str] = set()
    for line in (ROOT / path).read_text().splitlines():
        decl = _DECL.match(line)
        inst = _INSTANCE.match(line)
        if decl is not None:
            wires.add(unescape(decl[1]))
        elif inst is not None and inst[1] in CELL_NAMES:
            instances.add(unescape(inst[2]))
            types.add(inst[1])
    return wires, instances, types


def blif_nets(path: Path) -> set[str]:
    """Net names of the first model of a BLIF: ports, ``.subckt`` pins, ``.names`` outputs."""
    nets: set[str] = set()
    models = 0
    for line in (ROOT / path).read_text().splitlines():
        words = line.split()
        if not words or words[0].startswith("#"):
            continue
        models += words[0] == ".model"
        if models != 1:
            continue
        if words[0] in (".inputs", ".outputs"):
            nets.update(words[1:])
        elif words[0] == ".subckt":
            nets.update(m[1] for w in words[2:] if (m := _BLIF_PIN.match(w)) is not None)
        elif words[0] == ".names":
            nets.add(words[-1])
        elif words[0] == ".latch":
            nets.update(words[1:3])
            nets.update(words[4:5])  # the clock: `.latch IN OUT TYPE CTRL [INIT]`
    return nets


def missing(expected: set[str], rows: list[tuple[str, str, str, str]], module: str,
            kinds: tuple[str, ...]) -> list[str]:
    """The expected names the dump of ``module`` does not list under the kinds."""
    have = {name for mod, kind, name, _ in rows if mod == module and kind in kinds}
    return sorted(expected - have)


def check_names(args: argparse.Namespace) -> int:
    """G-3, G-4: cells stay cells; instance, wire and net names survive."""
    micro = micro_dir(args.micro)
    top = top_of(micro / "mapped.v")
    failures: list[str] = []
    rows, why = dump_lines(args.odin3, read_script(micro, "verilog"))
    if why:
        print(f"FAIL {args.micro} verilog: {why}")
        return EXIT_FAIL
    wires, instances, types = verilog_names(micro / "mapped.v")
    for what, found in (("wires", wires), ("instances", instances), ("cell types", types)):
        if not found:
            print(f"FAIL {args.micro}: no {what} found in {micro / 'mapped.v'} (the name "
                  f"extraction matched nothing, so the check would pass vacuously)")
            return EXIT_FAIL
    failures += [f"wire {n}" for n in missing(wires, rows, top, ("wire",))]
    failures += [f"instance {n}" for n in missing(instances, rows, top, ("node",))]
    modules = {name for _, kind, name, _ in rows if kind == "module"}
    failures += [f"cell module {n}" for n in sorted(types - modules)]
    brows, why = dump_lines(args.odin3, read_script(micro, "blif"))
    if why:
        print(f"FAIL {args.micro} blif: {why}")
        return EXIT_FAIL
    failures += [f"blif net {n}" for n in missing(blif_nets(micro / "mapped.blif"), brows, top,
                                                    ("net", "wire"))]
    print(f"netlist_dump {args.micro}: {len(wires)} wires, {len(instances)} instances, "
          f"{len(types)} cell types, {len(failures)} missing")
    for item in failures[:20]:
        print(f"FAIL missing {item}")
    return EXIT_FAIL if failures else EXIT_PASS


def check_prov(args: argparse.Namespace) -> int:
    """G-6: Verilog objects (module, wires, instances) carry a netlist location, BLIF objects
    none (IMPORTED)."""
    micro = micro_dir(args.micro)
    top = top_of(micro / "mapped.v")
    netlist_file = str(micro / "mapped.v")
    rows, why = dump_lines(args.odin3, read_script(micro, "verilog"))
    if why:
        print(f"FAIL {args.micro} verilog: {why}")
        return EXIT_FAIL
    own = [row for row in rows if row[0] == top and row[1] in ("module", "wire", "node")]
    if not any(row[1] == "wire" for row in own) or not any(row[1] == "node" for row in own):
        print(f"FAIL {args.micro} verilog: the dump of {top} lists no wire or no instance "
              f"(the check would pass vacuously)")
        return EXIT_FAIL
    bad = [row[2] for row in own if not row[3].startswith(netlist_file + ":")]
    brows, why = dump_lines(args.odin3, read_script(micro, "blif"))
    if why:
        print(f"FAIL {args.micro} blif: {why}")
        return EXIT_FAIL
    located = [row[2] for row in brows if row[0] == top and row[3] != ""]
    print(f"netlist_prov {args.micro}: verilog {len(own) - len(bad)}/{len(own)} with a "
          f"netlist location, blif {len(located)} with an HDL location")
    for name in bad[:10]:
        print(f"FAIL verilog object {name}: no location in {netlist_file}")
    for name in located[:10]:
        print(f"FAIL blif object {name}: has a location")
    return EXIT_FAIL if bad or located else EXIT_PASS


def check_dump(args: argparse.Namespace) -> int:
    """G-3, G-4 and G-6 on one micro: ``names`` then ``prov``."""
    return max(check_names(args), check_prov(args))


def check_src(args: argparse.Namespace) -> int:
    """G-6: the ``src`` fixture restores the RTL location with ``read_verilog -netlist``."""
    micro = micro_dir(args.micro)
    read = f"read_verilog --lib {CELLS}; read_verilog -netlist {micro / 'mapped_src.v'}"
    rows, why = dump_lines(args.odin3, read)
    if why:
        print(f"FAIL {args.micro} src: {why}")
        return EXIT_FAIL
    # The reader keeps the file name as the ``src`` attribute spells it (``rtl.v``, relative).
    rtl = "rtl.v"
    top = top_of(micro / "mapped_src.v")
    # ``src`` sits on the module, its wires and its instances; the derived nets and nodes (``$n``,
    # ``$c``) are not counted. A value that names only the techmap (``../cells/cmos_map.v``) has
    # no RTL location to restore, so only the values that start with ``rtl.v:`` count.
    restored = [row for row in rows
                if row[0] == top and row[1] in ("module", "wire", "node")
                and not row[2].startswith("$") and row[3].startswith(rtl + ":")]
    expected = len(_SRC_RTL.findall((ROOT / micro / "mapped_src.v").read_text()))
    print(f"netlist_src {args.micro}: {len(restored)} of {expected} src attributes with an "
          f"{rtl} location restored")
    if expected == 0:
        print(f"FAIL {micro / 'mapped_src.v'} has no rtl.v src attribute "
              f"(the check would be vacuous)")
        return EXIT_FAIL
    if len(restored) != expected:
        print(f"FAIL {len(restored)} objects have an {rtl} location, "
              f"{expected} src attributes exist")
        return EXIT_FAIL
    return EXIT_PASS


def check_unknown_directions(args: argparse.Namespace) -> list[str]:
    """EL-11: unknown cells on driven nets read; their ports get directions from the nets."""
    fixture = NETLIST / "unknown" / "unknown_drv.v"
    code, text = odin3(args.odin3, f"read_verilog --auto-blackbox {fixture}")
    failures = []
    if code != 0:
        failures.append(f"unknown_drv with --auto-blackbox: {first_error(text)}")
    for want in (
        "implicit black box 'MYSTERY': inferred port directions: A=input, B=input, Q=output; "
        "Q: chosen as output: the type had none",
        "implicit black box 'OTHER': inferred port directions: D=input, Q=output",
    ):
        if want not in text:
            failures.append(f"unknown_drv: missing warning: {want}")
    return failures


def check_unknown(args: argparse.Namespace) -> int:
    """G-7: an unknown cell is unresolved_module; --auto-blackbox makes it a black box."""
    fixture = NETLIST / "unknown" / "unknown.v"
    code, text = odin3(args.odin3, f"read_verilog {fixture}")
    failures = []
    if code == 0 or f"{fixture}:6:" not in text or "unresolved_module" not in text:
        failures.append("without --auto-blackbox: expected a located unresolved_module error")
    code, text = odin3(args.odin3, f"read_verilog --auto-blackbox {fixture}")
    if code != 0 or "implicit black box" not in text:
        failures.append(f"with --auto-blackbox: {first_error(text)}")
    failures.extend(check_unknown_directions(args))
    for failure in failures:
        print(f"FAIL {failure}")
    print(f"netlist_unknown: {len(failures)} failed")
    return EXIT_FAIL if failures else EXIT_PASS


def parser() -> argparse.ArgumentParser:
    """The command line."""
    top = argparse.ArgumentParser(prog="netlist-ingest", description=__doc__.split("\n")[0])
    top.add_argument("--odin3", required=True, help="the odin3 executable")
    top.add_argument("--equiv-check", default=str(ROOT / "tools/equiv-check/equiv-check"))
    sub = top.add_subparsers(dest="command", required=True)
    sub.add_parser("cells")
    sub.add_parser("unknown")
    equiv = sub.add_parser("equiv")
    equiv.add_argument("micro")
    equiv.add_argument("form", choices=FORMS)
    for name in ("dump", "names", "prov", "src"):
        sub.add_parser(name).add_argument("micro")
    return top


COMMANDS = {"cells": check_cells, "dump": check_dump, "equiv": check_equiv, "names": check_names,
            "prov": check_prov, "src": check_src, "unknown": check_unknown}


def main(argv: Sequence[str]) -> int:
    """Entry point."""
    args = parser().parse_args(argv)
    try:
        return COMMANDS[args.command](args)
    except memcap.CapUnavailable as exc:
        print(f"SKIP netlist-ingest {args.command} {getattr(args, 'micro', '')}: {exc}")
        return EXIT_SKIP
    except ToolError as exc:
        print(f"netlist-ingest: {exc}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
