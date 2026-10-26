#!/usr/bin/env python3
"""read-gate: the cost gate of V1.2.0 Task 4 (section 5.3).

Usage::

    read_gate.py --odin3 O [--repeats N] [--json OUT] <design.v>...

Runs ``read_techlib lib/vtr.o3lib; read_verilog --nolatches --vtr-compat --prov-ast MODE F``
N times (default 7) per design and mode, alternating ``off`` and ``refs`` (off first), smallest
design first, each process under ``flock -x /tmp/claude-1000/odin3-heavy.lock`` and
``/usr/bin/time -v``.  The time of a run is
the pass time odin3 logs (``pass read_verilog: X ms``: the read, elaboration and the snapshot);
the peak RSS is ``Maximum resident set size``.  Prints per design n, the median time of each mode,
the ratio, the peak RSS (median) of each mode, the delta in MB and PASS/FAIL (within +5% time AND
+10 MB), and writes every raw run to ``--json``.  Exit 0 all pass, 1 any fail.
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

LOCK = "/tmp/claude-1000/odin3-heavy.lock"
TECHLIB = Path(__file__).resolve().parents[2] / "lib" / "vtr.o3lib"
TIME_LIMIT = 1.05
RSS_LIMIT_MB = 10.0
PASS_RE = re.compile(r"pass read_verilog: ([0-9.]+) ms")
RSS_RE = re.compile(r"Maximum resident set size \(kbytes\): (\d+)")


def one_run(odin3: str, design: Path, mode: str) -> tuple[float, float]:
    """(pass time in ms, peak RSS in MB) of one read."""
    script = (
        f"read_techlib {TECHLIB}; read_verilog --nolatches --vtr-compat --prov-ast {mode} {design}"
    )
    proc = subprocess.run(
        ["flock", "-x", LOCK, "/usr/bin/time", "-v", odin3, "-p", script],
        capture_output=True, text=True, check=False)
    took, rss = PASS_RE.search(proc.stderr), RSS_RE.search(proc.stderr)
    if proc.returncode != 0 or took is None or rss is None:
        raise SystemExit(f"{design.name} {mode}: exit {proc.returncode}\n{proc.stderr[-400:]}")
    return float(took.group(1)), int(rss.group(1)) / 1024.0


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--odin3", required=True)
    ap.add_argument("--repeats", type=int, default=7)
    ap.add_argument("--json")
    ap.add_argument("designs", nargs="+")
    args = ap.parse_args(argv)
    designs = sorted((Path(d) for d in args.designs), key=lambda p: (p.stat().st_size, str(p)))
    raw: dict[str, dict[str, list[tuple[float, float]]]] = {}
    failed = 0
    print("design      n  off ms   refs ms  ratio  off MB  refs MB  delta MB  verdict")
    for design in designs:
        runs: dict[str, list[tuple[float, float]]] = {"off": [], "refs": []}
        for _ in range(args.repeats):
            for mode in ("off", "refs"):
                runs[mode].append(one_run(args.odin3, design, mode))
        raw[design.name] = runs
        t_off, t_refs = (statistics.median(r[0] for r in runs[m]) for m in ("off", "refs"))
        m_off, m_refs = (statistics.median(r[1] for r in runs[m]) for m in ("off", "refs"))
        ratio, delta = t_refs / t_off, m_refs - m_off
        ok = ratio <= TIME_LIMIT and delta <= RSS_LIMIT_MB
        failed += 0 if ok else 1
        print(f"{design.stem:<11}{args.repeats:>2} {t_off:>8.1f} {t_refs:>8.1f} {ratio:>6.3f} "
              f"{m_off:>7.1f} {m_refs:>8.1f} {delta:>9.2f}  {'PASS' if ok else 'FAIL'}")
    if args.json:
        Path(args.json).write_text(json.dumps(raw, indent=1, sort_keys=True) + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
