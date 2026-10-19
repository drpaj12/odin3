"""lower-gen-diff: seeds of the ``--lower`` generator through lower-diff and lower-equiv (3B spec
14.5, plan Task 9 step 5).

Usage::

    lower-gen-diff --odin3 O --yosys Y --seeds A..B [--expect FILE] [--work DIR] [--mem-cap C]
                   [--refine-timeout S] [--min-compared N] [--strict-unproven] [--parmys-techlibs D]

For each seed N in A..B, ``verilog_gen --lower`` (``lower_profile.generate``) writes
``DIR/seedN/seedN.v`` (the module is always ``t``; its first line names the *kind*), then:

1. ``lower-diff`` (tier by kind, spec 14.2: ``comb`` family ``arith`` tier T, ``ff`` family ``ff``
   tier D, ``latch`` family ``latch`` tier S, ``mem`` family ``mem``): Odin III's ``lower``
   against Yosys 0.55's ``techmap`` on the same ``in.il``. A difference fails the seed unless
   ``--expect FILE`` lists it (``tests/lower/gen_diff_expect.txt``: lower-diff's own
   ``<path> <family> <step> <reason>`` lines, ``path`` = ``seedN/seedN.v``);
2. ``lower-equiv``: ``lower`` refines its input, proved in Yosys 0.55 (the ``latch`` kind with
   ``--latches``). **A NOT A REFINEMENT is a bug in lower**: the seed is printed as ``BUG``, its
   directory is kept, and the run exits 1; it is never an expect-list entry.

A seed whose module ``opt`` folds to no word cell compares nothing: ``empty`` (counted; the run
fails below ``--min-compared``). An unproven result is a measured fact (a SAT size limit), counted
and printed with its reason; it never counts as ok, and ``--strict-unproven`` makes it exit 4.
Every seed's files stay under ``DIR`` (default: a new directory under the temporary one).

The last line is ``lower-gen-diff: seeds A..B: N ok, U unproven, E empty, D differ, B BUG``.

Exit 0 every seed ok, unproven or empty; 1 a difference or a BUG; 2 usage or setup error, or fewer
than ``--min-compared`` seeds compared; 3 a child crashed or timed out; 4 ``--strict-unproven`` and
some seed unproven; 77 skipped.
"""

from __future__ import annotations

import argparse
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
    diff: str = ""  # lower-diff: "ok", "differ", "unproven", "empty" or "error"
    equiv: str = ""  # lower-equiv: "ok", "bug", "unproven", "empty" or "error"
    notes: list[str] = field(default_factory=list)
    hard: bool = False

    @property
    def compared(self) -> bool:
        return self.diff == "ok" or self.equiv in ("ok", "bug")


def run_tool(argv: Sequence[str], cwd: Path) -> tuple[int, str]:
    """Runs one tool; (exit code, its output). The tools cap their own children."""
    done = subprocess.run(list(argv), cwd=cwd, capture_output=True, text=True, check=False)
    return done.returncode, (done.stdout or "") + (done.stderr or "")


def last_lines(text: str, count: int = 3) -> str:
    lines = [line for line in text.splitlines() if line.strip()]
    return " | ".join(lines[-count:])[:600]


def judge_diff(code: int, out: str, seed: Seed) -> None:
    if code == 0:
        seed.diff = "ok"
    elif code == lc.EXIT_FAIL and _NOTHING in out:
        seed.diff = "empty"
    elif code == lc.EXIT_FAIL and _UNPROVEN in out and "DIFFERS" not in out:
        seed.diff = "unproven"
        seed.notes.append(f"lower-diff: {last_lines(out, 2)}")
    elif code == lc.EXIT_FAIL:
        seed.diff = "differ"
        seed.notes.append(f"lower-diff: {last_lines(out)}")
    else:
        seed.diff = "error"
        seed.hard = code == lc.EXIT_CRASH
        seed.notes.append(f"lower-diff exit {code}: {last_lines(out)}")


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
        seed.hard = code == lc.EXIT_CRASH
        seed.notes.append(f"lower-equiv exit {code}: {last_lines(out)}")


def common_args(args: argparse.Namespace) -> list[str]:
    extra = ["--parmys-techlibs", args.parmys_techlibs] if args.parmys_techlibs else []
    return ["--odin3", args.odin3, "--yosys", args.yosys, "--top", TOP, "--mem-cap",
            args.mem_cap_text, *extra]


def run_seed(args: argparse.Namespace, number: int, work: Path) -> Seed:
    profile = lower_profile.generate(number)
    seed = Seed(number, profile.kind)
    folder = work / f"seed{number}"
    folder.mkdir(parents=True, exist_ok=True)
    source = folder / f"seed{number}.v"
    source.write_text(profile.text, encoding="utf-8")
    expect = ["--expect", str(args.expect.resolve()), "--root", str(work)] if args.expect else []
    code, out = run_tool([str(LOWER_DIFF), *common_args(args), *DIFF_ARGS[profile.kind], *expect,
                          "--work", str(folder / "diff"), str(source)], folder)
    judge_diff(code, out, seed)
    code, out = run_tool([str(LOWER_EQUIV), *common_args(args), *EQUIV_ARGS.get(profile.kind, ()),
                          "--refine-timeout", str(args.refine_timeout), "--allow-empty",
                          "--work", str(folder / "equiv"), str(source)], folder)
    judge_equiv(code, out, seed)
    return seed


def verdict_of(seed: Seed) -> str:
    """BUG, FAIL, unproven, empty or ok: one word per seed (the first that applies)."""
    if seed.equiv == "bug":
        return "BUG"
    if seed.diff in ("differ", "error") or seed.equiv == "error":
        return "FAIL"
    if "unproven" in (seed.diff, seed.equiv):
        return "unproven"
    return "empty" if seed.diff == "empty" else "ok"


def report(seed: Seed, folder: Path) -> str:
    verdict = verdict_of(seed)
    head = (f"{TOOL}: seed {seed.seed} [{seed.kind}]: {verdict} "
            f"(diff {seed.diff}, equiv {seed.equiv})")
    if verdict in ("ok", "empty"):
        return head
    return head + "".join(f"\n    {note}" for note in seed.notes) + f"\n    kept: {folder}"


def execute(args: argparse.Namespace, work: Path) -> int:
    seeds = parse_seeds(args.seeds)
    found: list[Seed] = []
    for number in seeds:
        result = run_seed(args, number, work)
        found.append(result)
        print(report(result, work / f"seed{number}"), flush=True)
        if result.hard:
            print(f"{TOOL}: HARD: seed {number}; work directory kept: {work}", file=sys.stderr)
            return lc.EXIT_CRASH
    count = Counter(verdict_of(s) for s in found)
    compared = sum(s.compared for s in found)
    print(f"{TOOL}: seeds {seeds.start}..{seeds.stop - 1}: {count['ok']} ok, "
          f"{count['unproven']} unproven, {count['empty']} empty, {count['FAIL']} differ, "
          f"{count['BUG']} BUG; compared {compared} of {len(found)}", flush=True)
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
    ap.add_argument("--min-compared", type=int, default=0, metavar="N",
                    help="fail unless at least N seeds were compared (not unproven, not empty)")
    ap.add_argument("--strict-unproven", action="store_true", help="an unproven seed exits 4")
    ap.add_argument("--work", type=Path, help="the work directory")
    return ap.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    try:
        lc.parse_size(args.mem_cap_text)
        work = lc.make_work(args.work, "lower-gen-diff-")
        return execute(args, work)
    except (OSError, lc.ToolError, GenDiffError, ValueError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return lc.EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
