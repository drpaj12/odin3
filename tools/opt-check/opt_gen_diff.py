"""opt-gen-diff: seeds of the ``--opt`` generator through opt-diff's steps 3-5 (2F spec 20.4,
plan Task 18).

Usage::

    opt-gen-diff --odin3 O --yosys Y --seeds A..B [--yosys-069 Y2] [--script S]...
                 [--expect-structural FILE] [--jobs 1] [--mem-cap C] [--work DIR]

For each seed N in A..B, ``verilog_gen --opt`` (``opt_profile.generate``) writes
``DIR/seedN/seedN.v`` (the module is always ``t``), then, for each ``S`` (default ``phase2`` and
``opt -full``), opt-diff's steps run on it: Odin III and Yosys 0.55 on the same RTLIL, (3) equal
per-module cell histograms, (4) ``yosys.il`` refined by ``odin.il`` and (5) identity after the
common lowering (``opt_diff.Diff``; the step numbers are §20.3's). Every seed's files stay under
``DIR`` (default: a new directory under the temporary one), so a failing seed's file is kept.

- A ``refine`` difference is a failure: ``seed N [S]: FAIL refine ...``, with the kept file.
- A ``histogram`` or ``identity`` difference is classified by the reverse refinement
  (``odin.il`` as gold): if Odin III's result and Yosys's refine each other it is ``structural``
  (the same behaviour in other cells or another order: a spec 17 row, or a rule one side lacks);
  if only Yosys's result is refined by Odin III's, Odin III resolved an x that Yosys kept
  (``refines only``). Both are reported with their rule counters and counted, and neither fails a
  seed by itself. With ``--expect-structural FILE`` (``tests/opt/gen_structural_expect.txt``: one
  ``SEED SCRIPT STEP | reason`` line per structural pair) a structural pair that is not listed
  fails the run, and a listed pair that no longer differs is reported as stale. Without the file
  (a long run over many seeds, for which no list is kept) every structural difference must still
  be proved, and the run fails when more than ``MAX_UNPROVED`` (5%) of its pairs differ without
  that proof (``refines only``: Odin III resolved an x that Yosys kept, or the reverse direction is
  not provable). A ``structural`` pair is proved in both directions by construction. Seeds 1..200
  and 1001..1050 gave 0 unproved pairs in 500, while the proved ones are 9-16% of the pairs and no
  soundness question, so they are reported but not gated.
- Only a crash of the oracle itself is an ``oracle crash``: Yosys dying (a signal, a cap kill or a
  timeout) on the ``in.il`` that Odin III wrote. It is counted and shown, and the run fails when
  more than ``MAX_CRASH`` (10%) of its pairs crash (so one where every Yosys child crashes fails).
  Everything else is a failure of ours or of the tools and fails the seed: a crash or timeout of
  Odin III, netlist-compare or a Yosys check of our own files exits 3; Yosys rejecting ``in.il`` or
  ``odin.il``, or Odin III exiting with an error, exits 1.
- ``--yosys-069 Y2`` (the nightly drift job): every (seed, script) also runs against Yosys 0.69; a
  different set of differing steps is ``oracle drift``, reported and counted, never a failure.

Exit 0 every seed ok, 1 a seed failed or too many differ unproved, 2 usage or setup error, 3 hard (a
non-oracle child crashed, was killed or timed out), 77 skipped.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from types import ModuleType

import memcap
import opt_diff
import opt_equiv

TOOL = "opt-gen-diff"
HERE = Path(__file__).resolve().parent
PROFILE_PY = HERE.parent / "verilog-gen" / "opt_profile.py"
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_CRASH, EXIT_SKIP = 0, 1, 2, 3, 77
GIB = 1 << 30
DEFAULT_CAP = 2 * GIB
TOP = "t"
DEFAULT_SCRIPTS = ("phase2", "opt -full")
MAX_UNPROVED = 0.05  # of the (seed, script) pairs, when no expected list is given
MAX_CRASH = 0.10  # of the (seed, script) pairs: Yosys children that crashed or timed out
_RANGE = re.compile(r"(\d+)\.\.(\d+)")


class GenDiffError(Exception):
    """A usage or setup error (exit 2)."""


def load_expected(path: Path) -> dict[tuple[int, str, str], str]:
    """``(seed, script slug, step) -> reason`` from an expected-structural file."""
    table: dict[tuple[int, str, str], str] = {}
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise GenDiffError(f"--expect-structural {path}: {exc.strerror}") from exc
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        head, bar, reason = line.partition("|")
        parts = head.split()
        if not bar or not reason.strip() or len(parts) < 3 or not parts[0].isdigit():
            raise GenDiffError(f"{path}:{number}: expected `SEED SCRIPT STEP | reason`")
        table[(int(parts[0]), slug_of(" ".join(parts[1:-1])), parts[-1])] = reason.strip()
    return table


def slug_of(name: str) -> str:
    return opt_diff.script_named(name).slug


@dataclass
class Pair:
    """The findings of one seed under one script."""

    script: str
    refine: str = ""  # the refine difference: a failure
    # step -> "structural" or "refines only"
    structural: dict[str, str] = field(default_factory=dict)
    steps: tuple[str, ...] = ()  # the differing steps against 0.55
    drift: str = ""
    crash: str = ""
    error: str = ""
    hard: bool = False  # the error is a crash, kill or timeout of a child other than the oracle
    counters: str = ""


def parse_seeds(text: str) -> range:
    match = _RANGE.fullmatch(text.strip())
    if match is None or int(match.group(1)) < 1 or int(match.group(2)) < int(match.group(1)):
        raise GenDiffError(f"--seeds {text!r}: expected A..B with 1 <= A <= B")
    return range(int(match.group(1)), int(match.group(2)) + 1)


def load_profile() -> ModuleType:
    import importlib.util
    spec = importlib.util.spec_from_file_location("opt_profile", PROFILE_PY)
    if spec is None or spec.loader is None:
        raise GenDiffError(f"{PROFILE_PY} is missing")
    module = importlib.util.module_from_spec(spec)
    sys.modules["opt_profile"] = module  # dataclasses looks its module up here
    spec.loader.exec_module(module)
    return module


def make_diff(args: argparse.Namespace, yosys: str, work: Path) -> opt_diff.Diff:
    ns = argparse.Namespace(odin3=args.odin3, yosys=yosys, top=TOP, techlib=None,
                            mem_cap=args.mem_cap, netlist_compare=opt_diff.NETLIST_COMPARE)
    return opt_diff.Diff(ns, work)


def oracle_trouble(exc: opt_equiv.ToolError) -> bool:
    """True only for a crash, kill or timeout of the oracle itself: Yosys running on the ``in.il``
    that Odin III wrote (step 2, ``what == "yosys"``). Yosys rejecting a file, and any failure of
    Odin III, netlist-compare or a Yosys check of our own files, are ours (review I2)."""
    return isinstance(exc, opt_equiv.CrashError) and exc.what == "yosys"  # not "yosys-refine"


def classify(diff: opt_diff.Diff, result: opt_diff.Result, run_dir: Path) -> Pair:
    pair = Pair(result.script.slug, steps=tuple(sorted(result.differences)))
    if "refine" in result.differences:
        pair.refine = result.differences["refine"]
    if pair.steps:
        pair.counters = (f"odin3: {opt_equiv.counter_text(result.odin_counters)}; "
                         f"yosys: {opt_equiv.counter_text(result.yosys_counters)}")
    for step in ("histogram", "identity"):
        if step in result.differences and not pair.refine:
            back, _ = diff.checker.refines(run_dir / "odin.il", run_dir / "yosys.il",
                                           f"{result.script.slug}.back")
            pair.structural[step] = "structural" if back else "refines only"
    return pair


def run_pair(args: argparse.Namespace, files: Sequence[str], seed_dir: Path,
             script: opt_diff.Script) -> Pair:
    diff = make_diff(args, args.yosys, seed_dir)
    try:
        result = diff.run(script, files)
    except opt_equiv.RefineAbort:
        raise  # our own check aborted: exit 2, never a failed seed
    except opt_equiv.ToolError as exc:
        pair = Pair(script.slug)
        if oracle_trouble(exc):
            pair.crash = str(exc)
        else:
            pair.error, pair.hard = str(exc), isinstance(exc, opt_equiv.CrashError)
        return pair
    try:
        pair = classify(diff, result, seed_dir / script.slug)
    except opt_equiv.CrashError as exc:  # the reverse refinement died: hard
        return Pair(script.slug, error=str(exc), hard=True)
    # any other ToolError of the reverse refinement (a Yosys ERROR) is not "refines only": it
    # propagates and ends the sweep with exit 2
    if args.yosys_069:
        pair.drift = drift_against(args, files, seed_dir, script, pair.steps)
    return pair


def drift_against(args: argparse.Namespace, files: Sequence[str], seed_dir: Path,
                  script: opt_diff.Script, steps: tuple[str, ...]) -> str:
    """Empty when Yosys 0.69 differs in the same steps as 0.55, else a one-line description."""
    folder = seed_dir / "yosys069"
    folder.mkdir(exist_ok=True)
    diff = make_diff(args, args.yosys_069, folder)
    try:
        result = diff.run(script, files)
    except opt_equiv.RefineAbort:
        raise  # our own check aborted: exit 2, never a failed seed
    except opt_equiv.ToolError as exc:
        return f"0.69 did not run: {str(exc)[:120]}"
    other = tuple(sorted(result.differences))
    if other == steps:
        return ""
    return f"0.55 differs in {list(steps) or 'nothing'}, 0.69 in {list(other) or 'nothing'}"


def report(seed: int, source: Path, pairs: Sequence[Pair],
           expected: dict[tuple[int, str, str], str] | None = None) -> bool:
    """Prints a seed's lines; True when it failed (an unlisted structural pair is a failure)."""
    failed = False
    for pair in pairs:
        tag = f"seed {seed} [{pair.script}]"
        if pair.error:
            print(f"{tag}: FAIL error: {pair.error}; file kept: {source}")
            failed = True
        elif pair.refine:
            print(f"{tag}: FAIL refine: {pair.refine}; file kept: {source}")
            print(f"  counters {pair.counters}")
            failed = True
        elif pair.crash:
            print(f"{tag}: oracle crash: {pair.crash}")
        else:
            notes = [f"{s}: {c}" for s, c in sorted(pair.structural.items())]
            if pair.drift:
                notes.append(f"oracle drift: {pair.drift}")
            print(f"{tag}: {'; '.join(notes) if notes else 'ok'}")
            if pair.structural:
                print(f"  counters {pair.counters}")
            if expected is not None:
                for step in pair.structural:
                    if (seed, pair.script, step) not in expected:
                        print(f"{tag}: FAIL unexpected structural {step}: not in the expected "
                              f"list; file kept: {source}")
                        failed = True
    return failed


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--seeds", required=True, metavar="A..B", help="the seeds, e.g. 1..200")
    ap.add_argument("--yosys-069", metavar="Y2", help="also run against this Yosys 0.69; a "
                    "different outcome is oracle drift")
    ap.add_argument("--script", action="append", default=[], metavar="S",
                    help="phase2 or an opt spelling; repeatable (default phase2, opt -full)")
    ap.add_argument("--mem-cap", type=opt_equiv.parse_size, default=DEFAULT_CAP, metavar="SIZE",
                    help="per child (default 2G)")
    ap.add_argument("--expect-structural", type=Path, metavar="FILE",
                    help="the structural (seed, script, step) pairs allowed; any other fails")
    ap.add_argument("--jobs", type=int, default=1, help="1 (the only value: one tool at a time)")
    ap.add_argument("--work", type=Path, help="keep every seed's files here")
    return ap.parse_args(argv)


def sweep(args: argparse.Namespace, seeds: range, work: Path) -> int:
    profile = load_profile()
    scripts = [opt_diff.script_named(n) for n in (args.script or list(DEFAULT_SCRIPTS))]
    expected = load_expected(args.expect_structural) if args.expect_structural else None
    seen: set[tuple[int, str, str]] = set()
    totals = {"failed": 0, "pairs": 0, "structural": 0, "unproved": 0, "drift": 0, "crash": 0,
              "hard": 0}
    for seed in seeds:
        seed_dir = work / f"seed{seed}"
        seed_dir.mkdir(parents=True, exist_ok=True)
        source = seed_dir / f"seed{seed}.v"
        source.write_text(profile.generate(seed).text, encoding="utf-8")
        pairs = [run_pair(args, [str(source)], seed_dir, s) for s in scripts]
        totals["failed"] += report(seed, source, pairs, expected)
        seen |= {(seed, p.script, step) for p in pairs for step in p.structural}
        totals["pairs"] += len(pairs)
        totals["structural"] += sum(bool(p.structural) for p in pairs)
        totals["unproved"] += sum("refines only" in p.structural.values() for p in pairs)
        totals["hard"] += sum(p.hard for p in pairs)
        totals["drift"] += sum(bool(p.drift) for p in pairs)
        totals["crash"] += sum(bool(p.crash) for p in pairs)
    print(f"{TOOL}: {len(seeds)} seeds, {totals['pairs']} runs, {totals['failed']} seeds failed, "
          f"{totals['structural']} structural ({totals['structural'] - totals['unproved']} proved "
          f"both ways, {totals['unproved']} not), {totals['drift']} oracle drift, "
          f"{totals['crash']} oracle crashes")
    if expected is not None:
        for key in sorted(k for k in expected if k[0] in seeds and k not in seen):
            print(f"{TOOL}: stale: seed {key[0]} [{key[1]}] {key[2]} is listed but no longer "
                  f"structural")
    if totals["hard"]:
        return EXIT_CRASH
    if totals["crash"] > MAX_CRASH * totals["pairs"]:
        print(f"{TOOL}: FAIL: {totals['crash']} of {totals['pairs']} runs had a Yosys crash "
              f"(the limit is {MAX_CRASH:.0%})")
        return EXIT_FAIL
    if expected is None and totals["unproved"] > MAX_UNPROVED * totals["pairs"]:
        print(f"{TOOL}: FAIL: {totals['unproved']} of {totals['pairs']} runs are structural and "
              f"not proved both ways (the limit is {MAX_UNPROVED:.0%})")
        return EXIT_FAIL
    return EXIT_FAIL if totals["failed"] else EXIT_OK


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = parse_args(sys.argv[1:] if argv is None else argv)
        seeds = parse_seeds(args.seeds)
        if args.jobs != 1:
            raise GenDiffError("--jobs: only 1 is supported")
        load_profile()
    except GenDiffError as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_USAGE
    except SystemExit as exc:  # argparse's usage error
        return exc.code if isinstance(exc.code, int) else EXIT_USAGE
    work = (args.work or Path(tempfile.mkdtemp(prefix="opt-gen-diff-"))).resolve()
    work.mkdir(parents=True, exist_ok=True)
    print(f"{TOOL}: seeds {args.seeds}, files under {work}")
    try:
        return sweep(args, seeds, work)
    except (opt_equiv.ToolError, GenDiffError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_USAGE
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP


if __name__ == "__main__":
    sys.exit(main())
