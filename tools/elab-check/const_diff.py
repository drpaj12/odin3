"""const-diff: random constant expressions, 2D's evaluator against Yosys 0.55's (2D spec 14.5).

Usage::

    const-diff --yosys Y --replay R --count N [--seed S] [--work DIR]

Draws N random cases (deterministic per seed) over every operator of Task 7's ``odin3_cop``
(``gen_const_table``'s generator: widths 1, 7, 64, 65 and 200, signed and unsigned, a third of the
operands holding x/z, and the ``--mixed`` kind with independent operand widths, signs and
unsized literals; half of N each), has Yosys 0.55 fold them (``assign y = <expr>;`` read back
from ``write_rtlil``), and replays the table through ``odin3_cval_op`` with the tiny driver
``const-replay`` (``R``). A case where the two disagree fails the run: the driver's
``const_table: <case>  got <bits>`` line names it. A Yosys whose ``-V`` is not 0.55 is refused.

``--work DIR`` keeps the two tables (``plain.txt``, ``mixed.txt``) there; without it they live
in a temporary directory that is removed. Exit 0 all agree, 1 any differs or Yosys fails, 2 usage
or setup error.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

import gen_const_table

TOOL = "const-diff"
EXIT_OK, EXIT_FAIL, EXIT_USAGE = 0, 1, 2
DEFAULT_SEED = 1
REPLAY_TIMEOUT_S = 600


def split_count(count: int) -> tuple[int, int]:
    """(plain, mixed): half each, the odd case to the plain table."""
    return count - count // 2, count // 2


def replay(driver: str, table: Path) -> tuple[int, str]:
    """(exit code, output) of the driver on one table."""
    try:
        done = subprocess.run([driver, str(table)], capture_output=True, text=True,
                              errors="replace", check=False, timeout=REPLAY_TIMEOUT_S)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return EXIT_USAGE, f"cannot run {driver}: {exc}"
    return done.returncode, done.stdout + done.stderr


def run_kind(args: argparse.Namespace, work: Path, mixed: bool, count: int) -> int:
    """One table: generate, replay, report. Returns the exit code."""
    if count == 0:
        return EXIT_OK
    name = "mixed" if mixed else "plain"
    seed = args.seed * 2 + (1 if mixed else 0)
    table = work / f"{name}.txt"
    table.write_text(gen_const_table.generate(args.yosys, seed, count, mixed), encoding="utf-8")
    code, out = replay(args.replay, table)
    print(f"{TOOL}: {name} (seed {seed}): {out.strip().splitlines()[-1] if out.strip() else ''}")
    if code != EXIT_OK:
        print(out.rstrip())
    return code


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--replay", required=True, help="the const-replay driver")
    ap.add_argument("--count", type=int, required=True, help="expressions to compare")
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED)
    ap.add_argument("--work", type=Path, help="keep the tables here")
    args = ap.parse_args(argv)
    if args.count < 1:
        ap.error("--count must be at least 1")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = parse_args(sys.argv[1:] if argv is None else argv)
    except SystemExit as exc:  # argparse's usage error
        return exc.code if isinstance(exc.code, int) else EXIT_USAGE
    plain, mixed = split_count(args.count)
    try:
        with tempfile.TemporaryDirectory(prefix="const-diff-") as tmp:
            work = args.work or Path(tmp)
            work.mkdir(parents=True, exist_ok=True)
            codes = [run_kind(args, work, False, plain), run_kind(args, work, True, mixed)]
    except gen_const_table.YosysRefused as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_USAGE
    except (gen_const_table.GenError, subprocess.TimeoutExpired, OSError) as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_FAIL
    worst = max(codes)
    print(f"{TOOL}: {args.count} expressions, {'all agree' if worst == EXIT_OK else 'FAIL'}")
    return worst


if __name__ == "__main__":
    sys.exit(main())
