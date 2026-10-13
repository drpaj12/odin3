"""run-rows-equiv: elab-equiv over the coverage rows of tests/elab/rows, smallest file first.

Each row of rows.txt runs through the bridge (spec 14.3) with the same Yosys 0.55 on both sides,
except the rows with more than one clock, which rely on their goldens (spec 14.2), and the rows
listed in equiv_expect_fail.txt (`<row> [needle] <reason>`), which must still fail with the needle in the bridge's output: a
listed row that now passes is reported as stale, one that fails differently as unexpected. Exit 0 every row as expected, 1 an unexpected result, 2 a usage or I/O
error, 77 the bridge skipped (no memory cap possible).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

TOOL = "run-rows-equiv"
EXIT_OK = 0
EXIT_FAIL = 1
EXIT_ERROR = 2
EXIT_SKIP = 77
ROW_LINE = re.compile(r"^([A-Z]\d+) (\S+\.v) comb=([01]) clocks=(\d+)$")


@dataclass(frozen=True)
class Row:
    row: str
    path: Path
    clocks: int


def read_rows(rows_dir: Path) -> list[Row]:
    """The rows of rows.txt, smallest source first (ties by row name)."""
    out = []
    for line in (rows_dir / "rows.txt").read_text(encoding="utf-8").splitlines():
        match = ROW_LINE.match(line)
        if match is None:
            raise ValueError(f"rows.txt: bad line {line!r}")
        out.append(Row(match.group(1), rows_dir / match.group(2), int(match.group(4))))
    return sorted(out, key=lambda r: (r.path.stat().st_size, r.row))


EXPECT_LINE = re.compile(r"^(\S+) (?:\[([^\]]+)\] )?(.+)$")


def read_expect_fail(path: Path) -> dict[str, str]:
    """`<row> <reason>` lines (blank lines and # comments skipped); a missing reason is an error."""
    return {row: reason for row, (_, reason) in read_expect(path).items()}


def read_expect(path: Path) -> dict[str, tuple[str, str]]:
    """`<row> [needle] <reason>` lines as {row: (needle, reason)}; the needle is optional."""
    out: dict[str, tuple[str, str]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        match = EXPECT_LINE.match(line.strip())
        if match is None:
            raise ValueError(f"{path.name}: {line.split(' ')[0]}: no reason")
        out[match.group(1)] = (match.group(2) or "", match.group(3).strip())
    return out


def run_row(bridge: Sequence[str], row: Row) -> tuple[int, str]:
    """The bridge's exit code and combined output on one row."""
    proc = subprocess.run([*bridge, str(row.path)], capture_output=True, text=True, check=False)
    out = proc.stdout + proc.stderr
    if proc.returncode not in (EXIT_OK, EXIT_SKIP):
        sys.stderr.write(out)
    return proc.returncode, out


def sweep(
    bridge: Sequence[str], rows: Sequence[Row], expect: dict[str, str],
    needles: dict[str, str] | None = None,
) -> int:
    bad = 0
    skipped = 0
    needles = needles or {}
    for row in rows:
        if row.clocks > 1:
            print(f"SKIP {row.row}: {row.clocks} clocks (golden only)")
            continue
        code, out = run_row(bridge, row)
        needle = needles.get(row.row, "")
        if code == EXIT_SKIP:
            skipped += 1
        elif code == EXIT_OK and row.row in expect:
            print(f"STALE {row.row}: listed in equiv_expect_fail.txt but equivalent")
            bad += 1
        elif code != EXIT_OK and row.row not in expect:
            print(f"FAIL {row.row}: bridge exit {code}")
            bad += 1
        elif code != EXIT_OK and needle not in out:
            print(f"FAIL {row.row}: bridge exit {code} without the expected [{needle}]")
            bad += 1
        else:
            print(f"ok {row.row}" + (" (expected failure)" if row.row in expect else ""))
    print(f"{TOOL}: {len(rows)} rows, {bad} unexpected, {skipped} skipped")
    if bad:
        return EXIT_FAIL
    return EXIT_SKIP if skipped else EXIT_OK


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n\n")[0])
    ap.add_argument("--elab-equiv", required=True, help="tools/elab-check/elab-equiv")
    ap.add_argument("--odin3", required=True)
    ap.add_argument("--yosys", required=True)
    ap.add_argument("--equiv-check", required=True)
    ap.add_argument("--rows", type=Path, required=True, help="tests/elab/rows")
    ap.add_argument("--expect-fail", type=Path, required=True, help="equiv_expect_fail.txt")
    ap.add_argument("--only", action="append", default=[], help="run only this row (repeatable)")
    args = ap.parse_args(argv)
    try:
        rows = read_rows(args.rows)
        detail = read_expect(args.expect_fail)
        expect = {row: reason for row, (_, reason) in detail.items()}
        needles = {row: needle for row, (needle, _) in detail.items()}
    except (OSError, ValueError) as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_ERROR
    if args.only:
        rows = [r for r in rows if r.row in args.only]
    bridge = [
        args.elab_equiv,
        "--odin3",
        args.odin3,
        "--yosys",
        args.yosys,
        "--equiv-check",
        args.equiv_check,
        "--unmap-enables",
    ]
    return sweep(bridge, rows, expect, needles)


if __name__ == "__main__":
    sys.exit(main())
