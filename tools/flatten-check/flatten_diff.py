"""flatten-diff: Odin III's `flatten` against Yosys 0.55's on one Verilog file (3A spec 14.3).

Both tools read the file, elaborate it with the top given, run ``proc; flatten`` and write RTLIL.
Module ``T`` of the two outputs is compared: (1) the public wire names and widths; (2) the public
cell names and the multiset of cell types (the instances that remain are cells whose type is a
module); (3) the MEMIDs of the memory cells; (4) connectivity: Yosys 0.55 proves Odin III's flat
module (gold) and its own (gate) equivalent with 2F's refinement script (``opt-equiv``'s
``Checker``: ``equiv_make``, ``equiv_simple -undef -seq 4``, ``equiv_induct``). ``--no-equiv`` skips
(4).

The expect file lists known differences, ``file: reason`` or ``file [kind,kind]: reason`` with the
kinds ``wire``, ``names``, ``cell-type``, ``memid``, ``equiv`` (no brackets: any kind). A difference
of another kind, or one in a file not listed, fails; a listed kind that no longer differs fails
too (a stale entry). Exit 0 / 1 / 2 as netlist-compare, 77 when no memory cap is available.
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

import flatten_common as fc
import memcap
import opt_equiv


def compare(
    odin: fc.RtlilModule, yosys: fc.RtlilModule, fold: Mapping[str, str] | None = None
) -> list[str]:
    """The differences between Odin III's flat module and Yosys's (empty: same). ``fold`` renames
    cell types before they are counted."""
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
    types_a, types_b = fc.cell_types(odin, fold), fc.cell_types(yosys, fold)
    for kind in sorted(types_a.keys() | types_b.keys()):
        if types_a[kind] != types_b[kind]:
            problems.append(f"cell type {kind}: odin3 {types_a[kind]} yosys {types_b[kind]}")
    if set(odin.memids) != set(yosys.memids):
        mine, theirs = sorted(set(odin.memids)), sorted(set(yosys.memids))
        problems.append(f"memid: odin3 {mine} yosys {theirs}")
    return problems


KINDS = ("wire", "names", "cell-type", "memid", "equiv")
_PREFIX_KIND = (
    ("wire ", "wire"),
    ("public cell names", "names"),
    ("cell type ", "cell-type"),
    ("memid", "memid"),
    ("equiv", "equiv"),
)


def kind_of(problem: str) -> str:
    """The kind of a problem line (for the expect file)."""
    for prefix, kind in _PREFIX_KIND:
        if problem.startswith(prefix):
            return kind
    return "other"


class MemoryChecker(opt_equiv.Checker):
    """The refinement check for files with memories: the last (``memory`` first) lowering only,
    since the others abort on a design that still holds a ``$mem``."""

    def flows(self, one_clock: bool = False) -> list[Sequence[str]]:
        return super().flows(one_clock)[-1:]


def has_memories(*paths: Path) -> bool:
    return any(" $mem" in path.read_text() for path in paths)


def equivalence(odin_il: Path, yosys_il: Path, top: str, yosys: str, work: Path) -> list[str]:
    """Problems of the equivalence leg: empty when Yosys proves Odin III's flat module equivalent
    to its own; otherwise one ``equiv: <class>`` line (``differs`` or ``unproven``)."""
    kind = MemoryChecker if has_memories(odin_il, yosys_il) else opt_equiv.Checker
    checker = kind(yosys, work, fc.CAP_BYTES, top=top)
    try:
        ok, tail = checker.refines(odin_il, yosys_il, "flatten")
    except opt_equiv.CrashError as exc:
        if not exc.timed_out:
            raise
        return ["equiv: unproven (timeout)"]
    except opt_equiv.RefineAbort as exc:
        return [f"equiv: unproven ({str(exc).splitlines()[0]})"]
    return [] if ok else [f"equiv: differs: {tail.splitlines()[-1] if tail else ''}"]


@dataclass
class Expected:
    """An expect-file entry: the kinds of difference it covers (empty: all) and why."""

    kinds: tuple[str, ...]
    reason: str

    def covers(self, problem: str) -> bool:
        return not self.kinds or kind_of(problem) in self.kinds


def load_expect(path: Path | None) -> dict[str, Expected]:
    """``file: reason`` and ``file [kind,kind]: reason`` lines of the expect file (# comments)."""
    expect: dict[str, Expected] = {}
    if path is None:
        return expect
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if ":" not in line:
            continue
        head, reason = line.split(":", 1)
        name, _, kinds = head.partition("[")
        wanted = tuple(k.strip() for k in kinds.rstrip("] ").split(",") if k.strip())
        expect[name.strip()] = Expected(wanted, reason.strip())
    return expect


def judge(problems: Sequence[str], entry: Expected | None) -> tuple[list[str], bool]:
    """(the unexpected problems, whether the entry is stale: it covers nothing that differs)."""
    if entry is None:
        return list(problems), False
    unexpected = [p for p in problems if not entry.covers(p)]
    stale = not any(entry.covers(p) for p in problems)
    return unexpected, stale


def scripts(
    source: Path, top: str, tools: fc.Tools, outs: tuple[Path, Path]
) -> tuple[str, str]:
    """The Odin III and the Yosys script (spec 14.3)."""
    compat = " --vtr-compat" if tools.vtr_compat else ""
    lib_a = f"read_techlib {tools.techlib}; " if tools.techlib else ""
    lib_b = f"read_verilog -nolatches {tools.yosys_lib}; " if tools.yosys_lib else ""
    script_a = (
        f"{lib_a}read_verilog{compat} --nolatches {source}; hierarchy --top {top}; proc; "
        f"flatten; write_rtlil {outs[0]}"
    )
    script_b = (
        f"{lib_b}read_verilog -nolatches {source}; hierarchy -top {top}; proc; "
        f"flatten -noscopeinfo; write_rtlil {outs[1]}"
    )
    return script_a, script_b


def diff_in(work: Path, source: Path, top: str, tools: fc.Tools, equiv: bool,
            fold: Mapping[str, str] | None = None) -> list[str] | str:
    """The differences of one file, run in the directory ``work``; an error string on failure."""
    out_a, out_b = work / "a.il", work / "b.il"
    script_a, script_b = scripts(source, top, tools, (out_a, out_b))
    run_a = fc.run_capped([tools.odin3, "--top", top, "-p", script_a])
    run_b = fc.run_capped([tools.yosys, "-q", "-p", script_b])
    if run_a.returncode != 0 or run_b.returncode != 0:
        return f"odin3 exit {run_a.returncode}, yosys exit {run_b.returncode}"
    mods_a, mods_b = fc.parse_rtlil(out_a.read_text()), fc.parse_rtlil(out_b.read_text())
    if top not in mods_a or top not in mods_b:
        return f"module {top} missing in an output"
    problems = compare(mods_a[top], mods_b[top], fold)
    if equiv:
        problems += equivalence(out_a, out_b, top, tools.yosys, work)
    return problems


def run_one(
    source: Path, top: str, tools: fc.Tools, keep: Path | None, equiv: bool = True
) -> list[str] | str:
    """The differences for one file, or an error string. With ``keep`` the RTLIL files stay in a
    new directory under it."""
    if keep is not None:
        return diff_in(Path(tempfile.mkdtemp(dir=keep)), source, top, tools, equiv)
    with tempfile.TemporaryDirectory() as tmp:
        return diff_in(Path(tmp), source, top, tools, equiv)


def report(name: str, result: list[str] | str, entry: Expected | None) -> int:
    """Prints the verdict of one file; returns its exit code."""
    if isinstance(result, str):
        print(f"{name}: error: {result}")
        return fc.EXIT_ERROR
    unexpected, stale = judge(result, entry)
    if unexpected:
        print(f"{name}: DIFFERS")
        for problem in unexpected:
            print(f"  {problem}")
        return fc.EXIT_DIFF
    if entry is not None and stale:
        print(f"{name}: stale expect entry ({entry.reason})")
        return fc.EXIT_DIFF
    print(f"{name}: {'expected difference' if entry is not None else 'same'}")
    return fc.EXIT_SAME


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(prog="flatten-diff", description=__doc__)
    parser.add_argument("--top", default="top")
    parser.add_argument("--yosys", default=os.environ.get("ODIN3_YOSYS055", "yosys"))
    parser.add_argument("--odin3", default="build/debug/odin3")
    parser.add_argument("--expect", type=Path, help="file[kinds]: reason lines")
    parser.add_argument("--keep", type=Path, help="directory for the RTLIL files (kept)")
    parser.add_argument("--no-equiv", action="store_true", help="skip the equivalence leg")
    parser.add_argument("--techlib", help="Odin III reads this .o3lib first (the hard blocks)")
    parser.add_argument("--yosys-lib", help="Yosys reads this Verilog file first (primitives.v)")
    parser.add_argument("--vtr-compat", action="store_true", help="read_verilog --vtr-compat")
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args(argv)
    expect = load_expect(args.expect)
    worst = fc.EXIT_SAME
    for source in args.files:
        try:
            tools = fc.Tools(
                args.odin3, args.yosys, args.techlib, args.yosys_lib, args.vtr_compat
            )
            result = run_one(source, args.top, tools, args.keep, not args.no_equiv)
        except memcap.CapUnavailable as exc:
            print(f"flatten-diff: skipped: {exc}")
            return fc.EXIT_SKIP
        except opt_equiv.ToolError as exc:
            result = str(exc)
        worst = max(worst, report(source.name, result, expect.get(source.name)))
    return worst


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
