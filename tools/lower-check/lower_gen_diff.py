"""lower-gen-diff: seeds of the ``--lower`` generator through lower-diff and lower-equiv (3B spec
14.5, plan Task 9 step 5).

Usage::

    lower-gen-diff --odin3 O --yosys Y --seeds A..B [--expect FILE] [--tier-t strict|report]
                   [--work DIR] [--mem-cap C] [--refine-timeout S] [--min-compared N]
                   [--strict-unproven] [--parmys-techlibs D]

For each seed N in A..B, ``verilog_gen --lower`` (``lower_profile.generate``) writes
``DIR/seedN/seedN.v`` (the module is always ``t``; its first line names the *kind*), then:

1. ``lower-diff`` (tier by kind, spec 14.2: ``comb`` family ``arith`` tier T, ``ff`` family ``ff``
   tier D, ``latch`` family ``latch`` tier S, ``mem`` family ``mem``): Odin III's ``lower``
   against Yosys 0.55's ``techmap`` on the same ``in.il``. Every difference fails the seed except a
   tier-T *histogram* difference (the same function, a few gates apart after ``opt -fast``;
   ``equiv-check`` passed): with ``--tier-t strict`` (default) it fails unless ``--expect FILE``
   lists the seed (``tests/lower/gen_diff_expect.txt``: ``seedN/seedN.v arith histogram
   <reason>``; the total gate-count delta must be at most ``MAX_LISTED_DELTA``, and a listed seed
   that no longer differs is stale and fails); with ``--tier-t report`` it is counted as
   ``structural`` and printed, never a failure (the long run: equivalence stays the pass criterion,
   spec 14.4). ``equiv``,
   ``identity`` and every other step are never absorbed;
2. ``lower-equiv``: ``lower`` refines its input, proved in Yosys 0.55 (the ``latch`` kind with
   ``--latches``). **A NOT A REFINEMENT is a bug in lower**: the seed is printed as ``BUG``, its
   directory is kept, and the run exits 1; it is never an expect-list entry.

A seed whose module ``opt`` folds to no word cell compares nothing: ``empty`` (counted; the run
fails below ``--min-compared``). An unproven result is a measured fact (a SAT size limit), counted
and printed with its reason; it never counts as ok, and ``--strict-unproven`` makes it exit 4.
Every seed's files stay under ``DIR`` (default: a new directory under the temporary one).

The last line is ``lower-gen-diff: seeds A..B: N ok, S structural, U unproven, E empty, D differ,
B BUG``.

Exit 0 every seed ok, structural (report mode), unproven or empty; 1 a difference or a BUG; 2 usage
or setup error (also a child's exit 2), or fewer than ``--min-compared`` seeds compared; 3 a child
crashed or timed out; 4 ``--strict-unproven`` and some seed unproven; 77 skipped (a child's exit 77,
or no ``VTR_ROOT`` / ``--parmys-techlibs`` for the flip-flop maps).
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

import lower_common as lc
import lower_profile

TOOL = "lower-gen-diff"
HERE = Path(__file__).resolve().parent
LOWER_DIFF = HERE / "lower-diff"
LOWER_EQUIV = HERE / "lower-equiv"
TOP = "t"
EXIT_UNPROVEN = 4
# kind -> lower-diff arguments (spec 14.2: the tier of the family; a module of mixed word cells has
# no gate-for-gate form, and a module of flip-flops holds the logic of its data input; equiv-check
# cannot take a clock that is an internal net, so lower-diff keeps the falling edge, while
# lower-equiv runs the default `--neg-edge invert`; a $dlatch is tier S, gate for gate).
DIFF_ARGS = {
    "comb": ("--family", "arith", "--tier", "T"),
    "ff": ("--family", "ff", "--tier", "D", "--lower-args=--neg-edge keep"),
    "latch": ("--family", "latch"),
    "mem": ("--family", "mem"),
}
EQUIV_ARGS = {"latch": ("--latches",)}
_RANGE = re.compile(r"(\d+)\.\.(\d+)")
_NOT_REFINEMENT = "NOT A REFINEMENT"
_NOTHING = "NOTHING TO COMPARE"
_UNPROVEN = "UNPROVEN"
MAX_LISTED_DELTA = 4  # gates, summed over the cell types, for a listed histogram difference
_COUNTS = re.compile(r"(\$_\w+_) (\d+)")
_HISTOGRAM = "histogram: DIFFERS"


class GenDiffError(Exception):
    """A usage or setup error (exit 2)."""


def parse_seeds(text: str) -> range:
    match = _RANGE.fullmatch(text.strip())
    if match is None or int(match.group(1)) < 1 or int(match.group(2)) < int(match.group(1)):
        raise GenDiffError(f"--seeds {text!r}: expected A..B with 1 <= A <= B")
    return range(int(match.group(1)), int(match.group(2)) + 1)


@dataclass
class Seed:
    """The findings of one seed."""

    seed: int
    kind: str
    # lower-diff: "ok", "listed", "structural", "differ", "unproven", "empty" or "error"
    diff: str = ""
    equiv: str = ""  # lower-equiv: "ok", "bug", "unproven", "empty" or "error"
    notes: list[str] = field(default_factory=list)
    hard: bool = False
    code: int = 0  # a child's exit 2 or 77, which ends the run

    @property
    def failed(self) -> bool:
        """A finding that exit 1 reports: a BUG or a difference (not a tool error or a skip)."""
        return self.equiv == "bug" or self.diff in ("differ", "stale")

    @property
    def compared(self) -> bool:
        return self.diff != "empty" and self.equiv in ("ok", "bug")


def run_tool(argv: Sequence[str], cwd: Path) -> tuple[int, str]:
    """Runs one tool; (exit code, its output). The tools cap their own children."""
    done = subprocess.run(list(argv), cwd=cwd, capture_output=True, text=True, check=False)
    return done.returncode, (done.stdout or "") + (done.stderr or "")


def last_lines(text: str, count: int = 3) -> str:
    lines = [line for line in text.splitlines() if line.strip()]
    return " | ".join(lines[-count:])[:600]


def gate_delta(line: str) -> int:
    """Sum over the cell types of |odin3 - yosys| in a lower-diff histogram line."""
    odin, _, yosys = line.partition("; yosys")
    left, right = dict(_COUNTS.findall(odin)), dict(_COUNTS.findall(yosys))
    return sum(abs(int(left.get(k, 0)) - int(right.get(k, 0))) for k in left.keys() | right.keys())


def problem_lines(out: str) -> list[str]:
    return [line for line in out.splitlines()
            if line.startswith("lower-diff:") and ": ok (" not in line]


def judge_diff(code: int, out: str, seed: Seed, tier_t: str = "strict",
               listed: str | None = None) -> None:
    """lower-diff's exit and output. A histogram-only difference (every problem line is the
    ``histogram`` step) is ``listed`` (``listed`` is the seed's expect-list reason and the gate
    delta is within MAX_LISTED_DELTA), ``structural`` (report mode) or ``differ``."""
    lines = problem_lines(out)
    if code == 0:
        seed.diff = "ok"
    elif code in (lc.EXIT_ERROR, lc.EXIT_SKIP):
        seed.code = code
        seed.diff = "error"
        seed.notes.append(f"lower-diff exit {code}: {last_lines(out)}")
    elif code == lc.EXIT_FAIL and lines and all(_NOTHING in x for x in lines):
        seed.diff = "empty"
    elif code == lc.EXIT_FAIL and lines and all(_UNPROVEN in x for x in lines):
        seed.diff = "unproven"
        seed.notes.append(f"lower-diff: {last_lines(out, 2)}")
    elif code == lc.EXIT_FAIL and lines and all(_HISTOGRAM in x for x in lines):
        judge_histogram(lines, seed, tier_t, listed)
    elif code == lc.EXIT_FAIL:
        seed.diff = "differ"
        seed.notes.append(f"lower-diff: {last_lines(out)}")
    else:
        seed.diff = "error"
        seed.hard = seed.hard or code == lc.EXIT_CRASH
        seed.notes.append(f"lower-diff exit {code}: {last_lines(out)}")


def judge_histogram(lines: list[str], seed: Seed, tier_t: str, listed: str | None) -> None:
    delta = sum(gate_delta(x) for x in lines)
    if listed is not None and delta <= MAX_LISTED_DELTA:
        seed.diff = "listed"
    elif tier_t == "report" and seed.kind == "comb":  # tier T is the comb kind's
        seed.diff = "structural"
        seed.notes.append(f"lower-diff: histogram differs by {delta} gates (equivalent)")
    else:
        seed.diff = "differ"
        over = f"{delta} gates, above the listed limit {MAX_LISTED_DELTA}"
        why = over if listed else "not listed"
        seed.notes.append(f"{lines[-1][:500]} ({why})")


def judge_equiv(code: int, out: str, seed: Seed) -> None:
    if code == 0:
        seed.equiv = "ok"
    elif code == lc.EXIT_FAIL and _NOT_REFINEMENT in out:
        seed.equiv = "bug"
        seed.notes.append(f"lower-equiv: {last_lines(out)}")
    elif code == lc.EXIT_FAIL and _NOTHING in out:
        seed.equiv = "empty"
    elif code == EXIT_UNPROVEN:
        seed.equiv = "unproven"
        seed.notes.append(f"lower-equiv: {last_lines(out, 2)}")
    else:
        seed.equiv = "error"
        seed.hard = seed.hard or code == lc.EXIT_CRASH
        seed.code = seed.code or (code if code in (lc.EXIT_ERROR, lc.EXIT_SKIP) else 0)
        seed.notes.append(f"lower-equiv exit {code}: {last_lines(out)}")


def common_args(args: argparse.Namespace) -> list[str]:
    extra = ["--parmys-techlibs", args.parmys_techlibs] if args.parmys_techlibs else []
    return ["--odin3", args.odin3, "--yosys", args.yosys, "--top", TOP, "--mem-cap",
            args.mem_cap_text, *extra]


def load_listed(path: Path | None) -> dict[str, str]:
    """``seedN/seedN.v`` to reason, from ``<path> <family> histogram <reason>`` lines."""
    found: dict[str, str] = {}
    if path is None:
        return found
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise GenDiffError(f"--expect {path}: {exc.strerror}") from exc
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split(None, 3)
        if len(fields) != 4 or fields[1] != "arith" or fields[2] != "histogram":
            raise GenDiffError(f"{path}:{number}: want '<seedN/seedN.v> arith histogram <reason>'")
        found[fields[0]] = fields[3]
    return found


def run_seed(args: argparse.Namespace, number: int, work: Path,
             listed: dict[str, str]) -> Seed:
    profile = lower_profile.generate(number)
    seed = Seed(number, profile.kind)
    folder = work / f"seed{number}"
    folder.mkdir(parents=True, exist_ok=True)
    source = folder / f"seed{number}.v"
    source.write_text(profile.text, encoding="utf-8")
    code, out = run_tool([str(LOWER_DIFF), *common_args(args), *DIFF_ARGS[profile.kind],
                          "--work", str(folder / "diff"), str(source)], folder)
    key = f"seed{number}/seed{number}.v"
    judge_diff(code, out, seed, args.tier_t, listed.get(key))
    if key in listed and seed.diff == "ok":
        seed.diff = "stale"
        seed.notes.append(f"lower-diff: {key} is in the expect list but no longer differs")
    code, out = run_tool([str(LOWER_EQUIV), *common_args(args), *EQUIV_ARGS.get(profile.kind, ()),
                          "--refine-timeout", str(args.refine_timeout), "--allow-empty",
                          "--work", str(folder / "equiv"), str(source)], folder)
    judge_equiv(code, out, seed)
    return seed


def verdict_of(seed: Seed) -> str:
    """BUG, FAIL, unproven, empty or ok: one word per seed (the first that applies)."""
    if seed.equiv == "bug":
        return "BUG"
    if seed.diff in ("differ", "error", "stale") or seed.equiv == "error":
        return "FAIL"
    if "unproven" in (seed.diff, seed.equiv):
        return "unproven"
    if seed.diff == "structural":
        return "structural"
    return "empty" if seed.diff == "empty" else "ok"


def report(seed: Seed, folder: Path) -> str:
    verdict = verdict_of(seed)
    head = (f"{TOOL}: seed {seed.seed} [{seed.kind}]: {verdict} "
            f"(diff {seed.diff}, equiv {seed.equiv})")
    if verdict in ("ok", "empty") or (verdict == "structural" and not seed.notes):
        return head
    return head + "".join(f"\n    {note}" for note in seed.notes) + f"\n    kept: {folder}"


def stop(found: list[Seed], last: Seed, work: Path) -> int:
    """A child's exit 2, 77 or a crash ends the run; the exit code keeps the order BUG or
    difference (1) > crash (3) > tool error (2) > skip (77), so that a later seed never masks an
    earlier finding."""
    what = "HARD: a child crashed or timed out" if last.hard else f"a child exited {last.code}"
    print(f"{TOOL}: seed {last.seed}: {what}; work kept: {work}", file=sys.stderr)
    if any(s.failed for s in found):
        return lc.EXIT_FAIL
    if last.hard:
        return lc.EXIT_CRASH
    return last.code


def execute(args: argparse.Namespace, work: Path) -> int:
    seeds = parse_seeds(args.seeds)
    found: list[Seed] = []
    listed = load_listed(args.expect)
    for number in seeds:
        result = run_seed(args, number, work, listed)
        found.append(result)
        print(report(result, work / f"seed{number}"), flush=True)
        if result.code or result.hard:
            return stop(found, result, work)
    count = Counter(verdict_of(s) for s in found)
    compared = sum(s.compared for s in found)
    print(f"{TOOL}: seeds {seeds.start}..{seeds.stop - 1}: {count['ok']} ok, "
          f"{count['structural']} structural, {count['unproven']} unproven, "
          f"{count['empty']} empty, {count['FAIL']} differ, "
          f"{count['BUG']} BUG; compared {compared} of {len(found)}", flush=True)
    # a listed seed is ok in the count: it differs as the list says
    if count["BUG"] or count["FAIL"]:
        return lc.EXIT_FAIL
    if compared < args.min_compared:
        print(f"{TOOL}: error: compared {compared}, below --min-compared {args.min_compared}",
              file=sys.stderr)
        return lc.EXIT_ERROR
    return EXIT_UNPROVEN if count["unproven"] and args.strict_unproven else lc.EXIT_OK


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--seeds", required=True, help="A..B")
    ap.add_argument("--expect", type=Path, help="tests/lower/gen_diff_expect.txt")
    ap.add_argument("--parmys-techlibs", metavar="DIR", help="Parmys's techlibs (ff maps)")
    ap.add_argument("--mem-cap", dest="mem_cap_text", default="3G", metavar="SIZE",
                    help="per child (default 3G)")
    ap.add_argument("--refine-timeout", type=float, default=60.0, metavar="S",
                    help="seconds for one SAT proof of lower-equiv (default 60)")
    ap.add_argument("--tier-t", choices=("strict", "report"), default="strict",
                    help="a tier-T histogram difference with equal function: strict (default) "
                    "fails unless --expect lists the seed; report counts it as structural")
    ap.add_argument("--min-compared", type=int, default=1, metavar="N",
                    help="fail unless at least N seeds were compared (not unproven, not empty)")
    ap.add_argument("--strict-unproven", action="store_true", help="an unproven seed exits 4")
    ap.add_argument("--work", type=Path, help="the work directory")
    return ap.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    if not args.parmys_techlibs and not os.environ.get("VTR_ROOT"):
        print(f"{TOOL}: skipped: VTR_ROOT is not set and no --parmys-techlibs (the flip-flop maps "
              "of lower-diff need Parmys's techlibs)", file=sys.stderr)
        return lc.EXIT_SKIP
    try:
        lc.parse_size(args.mem_cap_text)
        work = lc.make_work(args.work, "lower-gen-diff-")
        return execute(args, work)
    except (OSError, lc.ToolError, GenDiffError, ValueError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return lc.EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
