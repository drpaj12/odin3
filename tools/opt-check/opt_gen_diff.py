"""opt-gen-diff: seeds of the ``--opt`` generator through opt-diff's steps 3-5 (2F spec 20.4,
plan Task 18).

Usage::

    opt-gen-diff --odin3 O --yosys Y --seeds A..B [--yosys-069 Y2] [--script S]...
                 [--jobs 1] [--mem-cap C] [--work DIR]

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
  seed by itself: unlike a micro there is no ``diff_expect.txt`` line for a generated file. The run
  fails when more than ``MAX_STRUCTURAL`` of its (seed, script) pairs are structural.
- A Yosys child that crashes or times out is an ``oracle crash``: counted and shown, never a
  failure; an Odin III child that fails is a failure.
- ``--yosys-069 Y2`` (the nightly drift job): every (seed, script) also runs against Yosys 0.69; a
  different set of differing steps is ``oracle drift``, reported and counted, never a failure.

Exit 0 every seed ok, 1 a seed failed or too many are structural, 2 usage or setup error.
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
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_SKIP = 0, 1, 2, 77
GIB = 1 << 30
DEFAULT_CAP = 2 * GIB
TOP = "t"
DEFAULT_SCRIPTS = ("phase2", "opt -full")
MAX_STRUCTURAL = 0.25 # of the (seed, script) pairs
_RANGE = re.compile(r"(\d+)\.\.(\d+)")


class GenDiffError(Exception):
    """A usage or setup error (exit 2)."""


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


def oracle_trouble(message: str) -> bool:
    """True when an ``opt_diff`` ToolError names the Yosys child (an oracle crash or timeout)."""
    return message.startswith("yosys") or "timed out" in message


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
    except opt_equiv.ToolError as exc:
        pair = Pair(script.slug)
        if oracle_trouble(str(exc)):
            pair.crash = str(exc)
        else:
            pair.error = str(exc)
        return pair
    pair = classify(diff, result, seed_dir / script.slug)
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
    except opt_equiv.ToolError as exc:
        return f"0.69 did not run: {str(exc)[:120]}"
    other = tuple(sorted(result.differences))
    if other == steps:
        return ""
    return f"0.55 differs in {list(steps) or 'nothing'}, 0.69 in {list(other) or 'nothing'}"


def report(seed: int, source: Path, pairs: Sequence[Pair]) -> bool:
    """Prints a seed's lines; True when it failed."""
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
    ap.add_argument("--jobs", type=int, default=1, help="1 (the only value: one tool at a time)")
    ap.add_argument("--work", type=Path, help="keep every seed's files here")
    return ap.parse_args(argv)


def sweep(args: argparse.Namespace, seeds: range, work: Path) -> int:
    profile = load_profile()
    scripts = [opt_diff.script_named(n) for n in (args.script or list(DEFAULT_SCRIPTS))]
    totals = {"failed": 0, "pairs": 0, "structural": 0, "drift": 0, "crash": 0}
    for seed in seeds:
        seed_dir = work / f"seed{seed}"
        seed_dir.mkdir(parents=True, exist_ok=True)
        source = seed_dir / f"seed{seed}.v"
        source.write_text(profile.generate(seed).text, encoding="utf-8")
        pairs = [run_pair(args, [str(source)], seed_dir, s) for s in scripts]
        totals["failed"] += report(seed, source, pairs)
        totals["pairs"] += len(pairs)
        totals["structural"] += sum(bool(p.structural) for p in pairs)
        totals["drift"] += sum(bool(p.drift) for p in pairs)
        totals["crash"] += sum(bool(p.crash) for p in pairs)
    print(f"{TOOL}: {len(seeds)} seeds, {totals['pairs']} runs, {totals['failed']} seeds failed, "
          f"{totals['structural']} structural, {totals['drift']} oracle drift, "
          f"{totals['crash']} oracle crashes")
    if totals["structural"] > MAX_STRUCTURAL * totals["pairs"]:
        print(f"{TOOL}: FAIL: {totals['structural']} of {totals['pairs']} runs structural "
              f"(the limit is {MAX_STRUCTURAL:.0%})")
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
    except opt_equiv.ToolError as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_USAGE
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP


if __name__ == "__main__":
    sys.exit(main())
