#!/usr/bin/env python3
"""names-gate: time and peak memory of `names --check`, `--lookup` and `--chain` on a version 2 map
(V1.2.0 Task 6).

Usage::

    names_gate.py --odin3 O --map M --lookup SHORT [--repeats N] [--lock FILE]

For each of ``--check``, ``--lookup SHORT`` and ``--chain SHORT`` the pass runs N times (default
3), one invocation at a time, each under an exclusive ``flock`` on FILE (``--lock``, else
``ODIN3_HEAVY_LOCK``, else none) and ``/usr/bin/time -v``, with the ``vpr`` sampling thread of
export_gate.py (``run_locked``).  The table gives the pass time odin3 logs for ``names``, the wall
time, the peak RSS (all medians) and in how many samples a ``vpr`` process was seen.
"""

from __future__ import annotations

import argparse
import os
import re
import statistics
import sys
from collections.abc import Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import export_gate  # noqa: E402

PASS_RE = re.compile(r"pass names: ([0-9.]+) ms")


def measure(odin3: str, script: str, lock: str | None, repeats: int) -> dict[str, float | str]:
    """The medians of `repeats` runs of one names command."""
    runs: list[dict[str, float]] = []
    for _ in range(repeats):
        proc, samples = export_gate.run_locked(odin3, script, lock)
        took = PASS_RE.search(proc.stderr)
        rss = export_gate.RSS_RE.search(proc.stderr)
        if proc.returncode != 0 or took is None or rss is None:
            raise SystemExit(f"{script}: exit {proc.returncode}\n{proc.stderr[-600:]}")
        runs.append({"ms": float(took.group(1)), "mb": int(rss.group(1)) / 1024.0,
                     "wall": export_gate.wall_seconds(proc.stderr), "out": float(len(proc.stdout)),
                     **export_gate.vpr_fields(samples)})
    return {"ms": statistics.median(r["ms"] for r in runs),
            "wall": statistics.median(r["wall"] for r in runs),
            "mb": statistics.median(r["mb"] for r in runs),
            "vpr": export_gate.vpr_label(runs)}


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--odin3", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--lookup", required=True, help="a short name or N name to look up")
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--lock", default=os.environ.get("ODIN3_HEAVY_LOCK"))
    args = ap.parse_args(argv)
    print(f"map {args.map} ({Path(args.map).stat().st_size} bytes), n = {args.repeats}")
    print("command      pass ms    wall s   peak RSS MB  VPR processes")
    for label, option in (("--check", "--check"), ("--lookup", f"--lookup {args.lookup}"),
                          ("--chain", f"--chain {args.lookup}")):
        got = measure(args.odin3, f"names --map {args.map} {option}", args.lock, args.repeats)
        print(f"{label:<10}{got['ms']:>10.1f}{got['wall']:>10.2f}{got['mb']:>14.1f}  {got['vpr']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
