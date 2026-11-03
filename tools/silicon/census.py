#!/usr/bin/env python3
"""Census of instance changes between the OpenROAD step dumps of a run directory.

Usage: census.py OUTDIR   (reads OUTDIR/or/sNN_*.csv; writes census.csv; prints the table)
"""

from __future__ import annotations

import sys
from pathlib import Path

import silicon_flow


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print("usage: census.py OUTDIR", file=sys.stderr)
        return 2
    for row in silicon_flow.census(Path(argv[0])):
        print(row)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
