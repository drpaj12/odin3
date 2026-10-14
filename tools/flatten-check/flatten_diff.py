"""flatten-diff: Odin III's `flatten` against Yosys 0.55's on one Verilog file (3A spec 14.3).

Both tools read the file, elaborate it with the top given, run ``proc; flatten`` and write RTLIL.
Module ``T`` of the two outputs is compared: (1) the public wire names and widths; (2) the public
cell names and the multiset of cell types; (3) the instances that remain (cells whose type is a
module). A difference not listed in the expect file (``file: reason`` lines) fails; a listed file
that no longer differs fails too (a stale entry). Exit 0 / 1 / 2 as netlist-compare, 77 when no
memory cap is available.
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

import flatten_common as fc
import memcap


def compare(odin: fc.RtlilModule, yosys: fc.RtlilModule) -> list[str]:
    """The differences between Odin III's flat module and Yosys's (empty: same)."""
    problems: list[str] = []
    pub_a = {n: w for n, w in odin.wires.items() if not n.startswith("$")}
    pub_b = {n: w for n, w in yosys.wires.items() if not n.startswith("$")}
    for name in sorted(pub_a.keys() | pub_b.keys()):
        if pub_a.get(name) != pub_b.get(name):
            problems.append(f"wire {name}: odin3 {pub_a.get(name)} yosys {pub_b.get(name)}")
    names_a = sorted(n for _, n in odin.cells if not n.startswith("$"))
    names_b = sorted(n for _, n in yosys.cells if not n.startswith("$"))
    if names_a != names_b:
        problems.append(f"public cell names: odin3 {names_a} yosys {names_b}")
    types_a, types_b = fc.cell_types(odin), fc.cell_types(yosys)
    for kind in sorted(types_a.keys() | types_b.keys()):
        if types_a[kind] != types_b[kind]:
            problems.append(f"cell type {kind}: odin3 {types_a[kind]} yosys {types_b[kind]}")
    return problems


def load_expect(path: Path | None) -> dict[str, str]:
    """``file: reason`` lines of the expect file (comments with #)."""
    expect: dict[str, str] = {}
    if path is None:
        return expect
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if ":" in line:
            name, reason = line.split(":", 1)
            expect[name.strip()] = reason.strip()
    return expect


def run_one(source: Path, top: str, odin3: str, yosys: str, keep: Path | None) -> list[str] | str:
    """The differences for one file, or an error string."""
    with tempfile.TemporaryDirectory(dir=keep) as tmp:
        out_a, out_b = Path(tmp) / "a.il", Path(tmp) / "b.il"
        script_a = (
            f"read_verilog --nolatches {source}; hierarchy --top {top}; proc; flatten; "
            f"write_rtlil {out_a}"
        )
        script_b = (
            f"read_verilog -nolatches {source}; hierarchy -top {top}; proc; "
            f"flatten -noscopeinfo; write_rtlil {out_b}"
        )
        run_a = fc.run_capped([odin3, "-p", script_a])
        run_b = fc.run_capped([yosys, "-q", "-p", script_b])
        if run_a.returncode != 0 or run_b.returncode != 0:
            return f"odin3 exit {run_a.returncode}, yosys exit {run_b.returncode}"
        mods_a, mods_b = fc.parse_rtlil(out_a.read_text()), fc.parse_rtlil(out_b.read_text())
        if top not in mods_a or top not in mods_b:
            return f"module {top} missing in an output"
        return compare(mods_a[top], mods_b[top])


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(prog="flatten-diff", description=__doc__)
    parser.add_argument("--top", default="top")
    parser.add_argument("--yosys", default=os.environ.get("ODIN3_YOSYS055", "yosys"))
    parser.add_argument("--odin3", default="build/debug/odin3")
    parser.add_argument("--expect", type=Path, help="file: reason lines of known differences")
    parser.add_argument("--keep", type=Path, help="directory for the RTLIL files (kept)")
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args(argv)
    expect = load_expect(args.expect)
    worst = fc.EXIT_SAME
    for source in args.files:
        try:
            result = run_one(source, args.top, args.odin3, args.yosys, args.keep)
        except memcap.CapUnavailable as exc:
            print(f"flatten-diff: skipped: {exc}")
            return fc.EXIT_SKIP
        listed = source.name in expect
        if isinstance(result, str):
            print(f"{source.name}: error: {result}")
            worst = max(worst, fc.EXIT_ERROR)
        elif result and not listed:
            print(f"{source.name}: DIFFERS")
            for problem in result:
                print(f"  {problem}")
            worst = max(worst, fc.EXIT_DIFF)
        elif not result and listed:
            print(f"{source.name}: stale expect entry ({expect[source.name]})")
            worst = max(worst, fc.EXIT_DIFF)
        else:
            print(f"{source.name}: {'expected difference' if listed else 'same'}")
    return worst


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
