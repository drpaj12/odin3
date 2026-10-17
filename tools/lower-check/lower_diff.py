"""lower-diff: Odin III's lower against Yosys 0.55's on the same input (3B spec 14.2; plan Task 2).

Usage::

    lower-diff --odin3 O --yosys Y [--top T] [--family F] [--tier S|T|D] [--lower-args A]
               [--pre CMDS] [--expect F] [--root R] [--techlib L] [--parmys-techlibs DIR]
               [--parmys-exact] [--map M]... [--netlist-compare N] [--equiv-check E]
               [--mem-cap C] [--work DIR]
               [--each] [--verbose] FILE...

Per design (the FILEs together; with ``--each``, every FILE is its own design):

1. Odin III: ``read_verilog --nolatches FILE...; hierarchy; <pre>; write_rtlil in.il; lower <A>;
   write_rtlil lo.il; write_blif lo.blif; opt -fast; write_rtlil lo_opt.il`` (``pre`` defaults to
   ``proc; opt``, spec 14.2 step 1; a ``.blif`` FILE is ``read_blif``);
2. Yosys 0.55, on the same ``in.il``: ``read_rtlil in.il; [techmap -map M]...; techmap; dffunmap;
   opt_clean -purge; write_rtlil yo.il; write_blif -icells -true + vcc -false + gnd -undef
   + unconn yo.blif; opt -fast; write_rtlil yo_opt.il`` (``M`` are the declared-reference maps
   of the family, §6.3);
   Both BLIFs are compared without their unused constant generators (Yosys writes the false, true
   and undefined drivers always; ``lo.norm.blif``, ``yo.norm.blif``);
3. the verdict of the **tier** of the family (``--tier`` overrides it):

   - ``S`` (gate for gate; the ``simplemap`` family, the synchronous flip-flops, ``$dlatch``): the
     per-module cell-type histograms are equal (step ``histogram``) and ``netlist-compare``
     ``lo.blif yo.blif`` exits 0 (step ``identity``);
   - ``T`` (the ``techmap.v`` structures): ``equiv-check`` exits 0 (step ``equiv``) and the
     histograms of ``lo_opt.il`` and ``yo_opt.il``, both after ``opt -fast``, are equal (step
     ``histogram``; a listed difference is printed with its reason). An ``equiv-check`` that is
     inconclusive (its exit 4) is ``UNPROVEN``: printed, a failure like a difference unless an
     ``equiv`` line lists it, never ``ok``;
   - ``D`` (a declared reference: the asynchronous flip-flops, the other latches, ``$mem``): the
     reference is Yosys's lowering with the maps; ``equiv-check lo.blif yo.blif`` exits 0 (step
     ``equiv``).

A design whose ``in.il`` holds no word cell (``opt`` folded it away) compared nothing: it prints
``NOTHING TO COMPARE`` and fails (step ``compared``), and ``ok (N cells)`` names how many word
cells were lowered and compared. A difference fails unless ``--expect F`` lists it: lines
``<path> <family> <step> <reason>`` (``#`` comments), ``path`` relative to ``--root`` (else the
file name), ``family`` a family name or ``*``, ``step`` ``histogram``, ``identity``, ``equiv`` or
``compared``. A listed difference that does not occur fails too (a stale entry).
Yosys other than 0.55 is refused.

Exit codes (one convention across the opt tools): 0 every design agrees to its tier; 1 a
difference, an unproven check, nothing to compare, or a stale entry; 2 usage, or Odin III or Yosys
exited with an error; 3 hard: a child crashed, was killed by a signal or its memory cap, or timed
out (never absorbed by the expect list); 77 skipped (an ASan child and no systemd-run). The work
directory is made under ``$ODIN3_LOWER_WORK`` (else ``$TMPDIR``) and is never removed.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

import lower_common as lc
import memcap

TOOL = "lower-diff"
HERE = Path(__file__).resolve().parent
MAPS = HERE / "maps"
NETLIST_COMPARE = HERE.parent / "netlist-compare" / "netlist-compare"
EQUIV_CHECK = HERE.parent / "equiv-check" / "equiv-check"
STEPS = ("histogram", "identity", "equiv", "compared")
DEFAULT_PRE = ("proc", "opt")
# The latch fixtures: `opt` folds an active-low enable into the latch last and leaves the dead
# inverter cells of the condition; `opt_clean` removes them (Yosys's opt does both).
LATCH_PRE = ("proc", "opt", "opt_clean")
YOSYS_BLIF = "write_blif -icells -true + vcc -false + gnd -undef + unconn"
EQUIV_OK, EQUIV_DIFFERENT, EQUIV_ERROR, EQUIV_X, EQUIV_UNPROVEN, EQUIV_HARD = 0, 1, 2, 3, 4, 5
ASYNC_TYPES = frozenset({"$adff", "$adffe", "$aldff", "$aldffe", "$dffsr", "$dffsre",
                         "$adlatch", "$dlatchsr", "$sr", "$mem"})
# Families whose lowered netlist holds storage bit cells, which write_rtlil rejects (1F): the Odin
# III side is written as BLIF only and the histograms are read from the BLIFs.
BLIF_ONLY_FAMILIES = frozenset({"ff", "latch"})
VERSION_WORD = re.compile(r"Yosys\s+(\d+\.\d+)")


@dataclass(frozen=True)
class Family:
    """A cell family: its default tier and, for tier D, the maps of the declared reference."""

    name: str
    tier: str  # "S", "T", "D" or "auto" (D when the design holds an asynchronous cell type)
    maps: tuple[str, ...] = ()


# The maps of a name are looked up beside this tool (maps/) first, then in --parmys-techlibs.
FF_MAPS = ("adff2dff.v", "adffe2dff_reset_first.v", "aldff2dff.v", "aldffe2dff_reset_first.v",
           "dffsr2dff.v", "dffsre2dff.v")
# --parmys-exact: Parmys's own enable-over-reset maps in place of the reset-first ones (spec 16
# row 4).
PARMYS_EXACT = {"adffe2dff_reset_first.v": "adffe2dff.v",
                "aldffe2dff_reset_first.v": "aldffe2dff.v"}
FAMILIES = {
    "bitwise": Family("bitwise", "S"),
    "pmux": Family("pmux", "T"),
    "shift": Family("shift", "T"),
    "arith": Family("arith", "T"),
    "mul": Family("mul", "T"),
    "div": Family("div", "T"),
    "ff": Family("ff", "auto", FF_MAPS),
    "latch": Family("latch", "auto"),
    "mem": Family("mem", "D"),
}


def family_named(name: str) -> Family:
    if name not in FAMILIES:
        raise lc.ToolError(f"unknown family '{name}': one of {sorted(FAMILIES)}")
    return FAMILIES[name]


def tier_of(family: Family, forced: str | None, in_il: str) -> str:
    """S, T or D: ``--tier`` if given, else the family's, with ``auto`` deciding by the cell types
    ``in.il`` holds."""
    if forced is not None:
        return forced
    if family.tier != "auto":
        return family.tier
    return "D" if lc.cell_types(in_il) & ASYNC_TYPES else "S"


def default_pre(family: Family) -> tuple[str, ...]:
    return LATCH_PRE if family.name == "latch" else DEFAULT_PRE


def resolve_map(name: str, parmys_dir: Path | None) -> Path:
    """The path of a map: ``maps/<name>`` of this tool, else ``<parmys-techlibs>/<name>``."""
    own = MAPS / name
    if own.exists():
        return own
    if parmys_dir is not None and (parmys_dir / name).exists():
        return parmys_dir / name
    where = f"{MAPS} and {parmys_dir}" if parmys_dir else str(MAPS)
    raise lc.ToolError(f"map {name} not found (looked in {where}; pass --parmys-techlibs)")


@dataclass(frozen=True)
class Plan:
    """What one design runs: the Odin III front, ``pre`` commands and ``lower`` arguments."""

    files: tuple[str, ...]
    top: str | None
    pre: tuple[str, ...]
    lower_args: str
    techlib: str | None
    rtlil_out: bool = True
    nolatches: bool = True  # False for the latch family: the fixtures need the inferred latches


def odin_script(plan: Plan, work: Path) -> str:
    """Step 1's Odin III ``-p`` script."""
    front = lc.read_front(plan.files, plan.top, plan.techlib, plan.nolatches)
    lower = f"lower {plan.lower_args}".strip()
    if not plan.rtlil_out:  # the storage bit cells have no RTLIL form (1F): BLIF only
        return "; ".join([*front, *plan.pre, f"write_rtlil {work / 'in.il'}", lower,
                          f"write_blif {work / 'lo.blif'}"])
    return "; ".join([*front, *plan.pre, f"write_rtlil {work / 'in.il'}", lower,
                      f"write_rtlil {work / 'lo.il'}", f"write_blif {work / 'lo.blif'}",
                      "opt -fast", f"write_rtlil {work / 'lo_opt.il'}"])


def yosys_script(work: Path, maps: Sequence[Path]) -> str:
    """Step 2's Yosys 0.55 ``-p`` script."""
    return "; ".join([f"read_rtlil {work / 'in.il'}", *(f"techmap -map {m}" for m in maps),
                      "techmap", "dffunmap", "opt_clean -purge", f"write_rtlil {work / 'yo.il'}",
                      f"{YOSYS_BLIF} {work / 'yo.blif'}", "opt -fast",
                      f"write_rtlil {work / 'yo_opt.il'}"])


@dataclass(frozen=True)
class Expect:
    reason: str


def parse_expect(text: str) -> dict[tuple[str, str, str], Expect]:
    """``<path> <family> <step> <reason>`` lines; ``ToolError`` on a malformed one."""
    found: dict[tuple[str, str, str], Expect] = {}
    names = {"*", *FAMILIES}
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split(None, 3)
        if len(fields) != 4 or fields[1] not in names or fields[2] not in STEPS:
            raise lc.ToolError(
                f"expect line {number}: want '<path> <family> <step> <reason>' with family in "
                f"{sorted(names)} and step in {STEPS}, got '{raw.strip()}'")
        found[(fields[0], fields[1], fields[2])] = Expect(fields[3])
    return found


def expected(key: str, family: str, step: str, table: dict[tuple[str, str, str], Expect]
             ) -> Expect | None:
    return table.get((key, family, step)) or table.get((key, "*", step))


@dataclass
class Result:
    """The findings of one design: step to difference text, the tier and unproven notes."""

    family: str
    tier: str = ""
    differences: dict[str, str] = field(default_factory=dict)
    unproven: str = ""
    cells: int = 0  # word cells in in.il: what lower had to lower and the tools to compare


class Diff:
    """One design through §14.2's steps."""

    def __init__(self, args: argparse.Namespace, work: Path) -> None:
        self.args = args
        self.work = work
        self.cap: int = args.mem_cap
        self.techlib = str(Path(args.techlib).resolve()) if args.techlib else None
        self.parmys = Path(args.parmys_techlibs) if args.parmys_techlibs else vtr_techlibs()

    def child(self, what: str, argv: Sequence[str], run_dir: Path,
              env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        return lc.capped(what, argv, self.cap, cwd=run_dir, env=env)

    def must(self, what: str, argv: Sequence[str], run_dir: Path) -> str:
        done = self.child(what, argv, run_dir)
        log = (done.stdout or "") + (done.stderr or "")
        if done.returncode != 0:
            raise lc.ToolError(f"{what} exit {done.returncode}: {lc.why(log)}")
        return log

    def run(self, files: Sequence[str], run_dir: Path) -> Result:
        args = self.args
        family = family_named(args.family)
        plan = Plan(tuple(files), args.top, tuple(args.pre or default_pre(family)),
                    args.lower_args or "", self.techlib, family.name not in BLIF_ONLY_FAMILIES,
                    family.name != "latch")
        self.must("odin3", [lc.exe(args.odin3), *lc.odin_args(args.top), "-p",
                            odin_script(plan, run_dir)], run_dir)
        in_il = (run_dir / "in.il").read_text(encoding="utf-8")
        result = Result(family.name, tier_of(family, args.tier, in_il), cells=word_cells(in_il))
        names = [PARMYS_EXACT.get(n, n) if args.parmys_exact else n for n in family.maps]
        maps = [resolve_map(n, self.parmys) for n in names] + [Path(m) for m in args.map]
        self.must("yosys", [lc.exe(args.yosys), "-p", yosys_script(run_dir, maps)], run_dir)
        self.judge(result, run_dir)
        return result

    def normalise(self, run_dir: Path) -> None:
        """lo.blif and yo.blif without unused constant generators, as lo.norm.blif/yo.norm.blif."""
        for name in ("lo", "yo"):
            text = (run_dir / f"{name}.blif").read_text(encoding="utf-8")
            text = lc.strip_unused_constants(text)
            text = lc.storage_to_latch(text) if name == "yo" else text
            (run_dir / f"{name}.norm.blif").write_text(text, encoding="utf-8")

    def judge(self, result: Result, run_dir: Path) -> None:
        self.normalise(run_dir)
        tier = result.tier
        if tier in ("S", "T"):
            raw = tier == "S"
            if result.family in BLIF_ONLY_FAMILIES:
                odin = lc.blif_histogram((run_dir / "lo.norm.blif").read_text("utf-8"))
                yosys = lc.blif_histogram((run_dir / "yo.norm.blif").read_text("utf-8"))
            else:
                lo_il = run_dir / ("lo.il" if raw else "lo_opt.il")
                yo_il = run_dir / ("yo.il" if raw else "yo_opt.il")
                odin = lc.histogram(lo_il.read_text("utf-8"))
                yosys = lc.histogram(yo_il.read_text("utf-8"))
            diff = lc.histogram_diff(odin, yosys)
            if diff:
                result.differences["histogram"] = diff
        if tier == "S":
            self.identity(result, run_dir)
        else:
            self.equiv(result, run_dir)

    def identity(self, result: Result, run_dir: Path) -> None:
        done = self.child("netlist-compare", [self.args.netlist_compare, "--quiet",
                                              "--implicit-models", str(run_dir / "lo.norm.blif"),
                                              str(run_dir / "yo.norm.blif")], run_dir)
        if done.returncode == 1:
            result.differences["identity"] = "netlist-compare: not identical"
        elif done.returncode != 0:
            raise lc.ToolError(f"netlist-compare cannot compare them (exit {done.returncode}): "
                               f"{lc.why((done.stdout or '') + (done.stderr or ''))}")

    def equiv(self, result: Result, run_dir: Path) -> None:
        for name in ("lo", "yo"):
            text = (run_dir / f"{name}.norm.blif").read_text(encoding="utf-8")
            (run_dir / f"{name}.eq.blif").write_text(lc.with_gate_models(text), encoding="utf-8")
        done = self.child("equiv-check", [self.args.equiv_check, str(run_dir / "lo.eq.blif"),
                                          str(run_dir / "yo.eq.blif")], run_dir)
        out = (done.stdout or "") + (done.stderr or "")
        if done.returncode == EQUIV_OK:
            return
        if done.returncode == EQUIV_DIFFERENT:
            result.differences["equiv"] = f"equiv-check: {lc.why(out)}"
        elif done.returncode == EQUIV_UNPROVEN:
            result.unproven = lc.why(out) or "no verdict"
        elif done.returncode == EQUIV_HARD:
            raise lc.CrashError("equiv-check", f"hard (exit 5): {lc.why(out)}")
        else:
            raise lc.ToolError(f"equiv-check exit {done.returncode}: {lc.why(out)}")


def word_cells(rtlil: str) -> int:
    """Word cells (``$name``, not ``$_NAME_``) of an RTLIL text: the cells ``lower`` lowers."""
    return sum(n for counts in lc.histogram(rtlil).values() for name, n in counts.items()
               if name.startswith("$") and not name.startswith("$_"))


def vtr_techlibs() -> Path | None:
    root = os.environ.get("VTR_ROOT")
    if not root:
        return None
    found = Path(root) / "parmys" / "parmys-plugin" / "techlibs"
    return found if found.is_dir() else None


def check_yosys(yosys: str, work: Path, cap: int) -> None:
    """Refuses a Yosys that is not 0.55 (the oracle is pinned by VTR)."""
    done = lc.capped("yosys -V", [lc.exe(yosys), "-V"], cap, cwd=work)
    found = VERSION_WORD.search((done.stdout or "") + (done.stderr or ""))
    if done.returncode != 0 or found is None or found.group(1) != lc.YOSYS_VERSION:
        got = found.group(1) if found else "an unknown version"
        raise lc.ToolError(f"{yosys} is Yosys {got}; the oracle is Yosys {lc.YOSYS_VERSION}")


def verdict(name: str, key: str, res: Result, table: dict[tuple[str, str, str], Expect],
            verbose: bool) -> tuple[list[str], bool]:
    """The output lines of one design and whether it failed (a difference, an unproven check or an
    empty comparison nobody listed, or a listed one that did not occur)."""
    lines: list[str] = []
    failed = False
    tag = f"{TOOL}: {name} [{res.family}/{res.tier}]"
    found_by_step = dict(res.differences)
    if res.unproven:
        found_by_step["equiv"] = f"UNPROVEN: {res.unproven}"
    if res.cells == 0:
        found_by_step["compared"] = "NOTHING TO COMPARE: in.il holds no word cell (folded away?)"
    for step in STEPS:
        reason = expected(key, res.family, step, table)
        found = found_by_step.get(step)
        if found is None and reason is not None:
            lines.append(f"{tag}: {step}: stale entry in the expect list ({reason.reason})")
            failed = True
        elif found is not None and reason is None:
            shown = found if found.startswith(("UNPROVEN", "NOTHING")) else f"DIFFERS: {found}"
            lines.append(f"{tag}: {step}: {shown}")
            failed = True
        elif found is not None and reason is not None:
            lines.append(f"{tag}: {step}: differs as listed ({reason.reason})")
    if not found_by_step and not any(expected(key, res.family, s, table) for s in STEPS):
        lines.append(f"{tag}: ok ({res.cells} cells)")
    return lines, failed


def rel_key(files: Sequence[str], root: Path | None) -> str:
    """The expect-list key of a design: the first file, relative to ``root`` when under it."""
    path = Path(files[0]).resolve()
    if root is not None and root.resolve() in path.parents:
        return str(path.relative_to(root.resolve()))
    return path.name


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--top", help="the top module")
    ap.add_argument("--family", default="bitwise", help=f"one of {sorted(FAMILIES)}")
    ap.add_argument("--tier", choices=("S", "T", "D"), help="override the family's tier")
    ap.add_argument("--lower-args", default="", metavar="A", help="arguments of the lower pass")
    ap.add_argument("--pre", action="append", metavar="CMD",
                    help="a command between hierarchy and lower (repeatable; default proc, opt)")
    ap.add_argument("--expect", type=Path, help="tests/lower/diff_expect.txt")
    ap.add_argument("--root", type=Path, help="expect-list paths are relative to this directory")
    ap.add_argument("--techlib", help="read this tech library (.o3lib) first")
    ap.add_argument("--parmys-techlibs", metavar="DIR",
                    help="Parmys's techlibs (default $VTR_ROOT/parmys/parmys-plugin/techlibs)")
    ap.add_argument("--parmys-exact", action="store_true",
                    help="use Parmys's unmodified adffe2dff.v / aldffe2dff.v (the enable over the "
                         "reset, spec 16 row 4) instead of the reset-first maps")
    ap.add_argument("--map", action="append", default=[], metavar="M",
                    help="an extra techmap file for the Yosys side (repeatable)")
    ap.add_argument("--netlist-compare", default=str(NETLIST_COMPARE), metavar="N")
    ap.add_argument("--equiv-check", default=str(EQUIV_CHECK), metavar="E")
    ap.add_argument("--mem-cap", type=lc.parse_size, default=lc.DEFAULT_CAP, metavar="SIZE",
                    help="per child (default 3G)")
    ap.add_argument("--work", type=Path, help="the work directory")
    ap.add_argument("--each", action="store_true", help="every FILE is its own design")
    ap.add_argument("--verbose", action="store_true", help="print the histograms of every design")
    ap.add_argument("files", nargs="+", help="the Verilog (or BLIF) files")
    return ap.parse_args(argv)


def execute(args: argparse.Namespace, work: Path) -> int:
    files = [str(Path(f).resolve()) for f in args.files]
    designs = [[f] for f in files] if args.each else [files]
    table = parse_expect(args.expect.read_text(encoding="utf-8")) if args.expect else {}
    check_yosys(args.yosys, work, args.mem_cap)
    diff = Diff(args, work)
    failed = False
    for number, design in enumerate(designs):
        run_dir = work / f"d{number:04d}"
        run_dir.mkdir(exist_ok=True)
        res = diff.run(design, run_dir)
        lines, bad = verdict(Path(design[0]).name, rel_key(design, args.root), res, table,
                             args.verbose)
        for line in lines:
            print(line, flush=True)
        failed = failed or bad
    return lc.EXIT_FAIL if failed else lc.EXIT_OK


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    work = lc.make_work(args.work, "lower-diff-")
    try:
        return execute(args, work)
    except lc.CrashError as exc:
        print(f"{TOOL}: HARD: {exc}", file=sys.stderr)
        print(f"{TOOL}: work directory kept: {work}", file=sys.stderr)
        return lc.EXIT_CRASH
    except (OSError, lc.ToolError, ValueError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return lc.EXIT_ERROR
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return lc.EXIT_SKIP


if __name__ == "__main__":
    sys.exit(main())
