#!/usr/bin/env python3
"""read-gate: the cost gate of V1.2.0 Task 4 (section 5.3).

Usage::

    read_gate.py --odin3 O [--repeats N] [--json OUT] [--techlib FILE] [--lock FILE] <design.v>...
    read_gate.py --from-json RAW.json

Runs ``read_techlib TECHLIB; read_verilog --nolatches --vtr-compat --prov-ast MODE F``
N times (default 7) per design and mode, alternating ``off`` and ``refs`` (off first), smallest
design first, each process under ``flock -x LOCK`` (``--lock``, else the environment variable
``ODIN3_HEAVY_LOCK``, else no lock) and ``/usr/bin/time -v``.  TECHLIB is ``--techlib``, else
``ODIN3_TECHLIB``, else ``lib/vtr.o3lib`` of this repository.  The time of a run is the pass
time odin3 logs (``pass read_verilog: X ms``: the read, elaboration and the snapshot); the peak RSS
is ``Maximum resident set size``.  Prints per design n, the median, standard deviation and minimum
time of each mode, the ratio of the medians, the peak RSS (median) of each mode, the delta in MB
and PASS/FAIL (within +5% time AND +10 MB), and writes every raw run to ``--json``.
``--from-json`` prints the same table from a raw file, with no run.  Exit 0 all pass, 1 any fail.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

DEFAULT_TECHLIB = Path(__file__).resolve().parents[2] / "lib" / "vtr.o3lib"
TIME_LIMIT = 1.05
RSS_LIMIT_MB = 10.0
PASS_RE = re.compile(r"pass read_verilog: ([0-9.]+) ms")
RSS_RE = re.compile(r"Maximum resident set size \(kbytes\): (\d+)")


Runs = dict[str, dict[str, list[tuple[float, float]]]]


def one_run(
    odin3: str, design: Path, mode: str, env: tuple[Path, str | None]
) -> tuple[float, float]:
    """(pass time in ms, peak RSS in MB) of one read; env is (techlib, lock file or None)."""
    script = (
        f"read_techlib {env[0]}; read_verilog --nolatches --vtr-compat --prov-ast {mode} {design}"
    )
    lock = ["flock", "-x", env[1]] if env[1] else []
    proc = subprocess.run(
        [*lock, "/usr/bin/time", "-v", odin3, "-p", script],
        capture_output=True, text=True, check=False)
    took, rss = PASS_RE.search(proc.stderr), RSS_RE.search(proc.stderr)
    if proc.returncode != 0 or took is None or rss is None:
        raise SystemExit(f"{design.name} {mode}: exit {proc.returncode}\n{proc.stderr[-400:]}")
    return float(took.group(1)), int(rss.group(1)) / 1024.0


def spread(values: list[float]) -> tuple[float, float, float]:
    """(median, sample standard deviation, minimum); the deviation is 0 for one value."""
    sd = statistics.stdev(values) if len(values) > 1 else 0.0
    return statistics.median(values), sd, min(values)


def report(raw: Runs) -> int:
    """Prints the table of the raw runs; the number of designs that fail the gate."""
    failed = 0
    print("design            n  off ms (sd, min)        refs ms (sd, min)       ratio  "
          "off MB  refs MB  delta MB  verdict")
    for name, runs in raw.items():
        n = len(runs["off"])
        t_off, t_refs = (spread([r[0] for r in runs[m]]) for m in ("off", "refs"))
        m_off, m_refs = (statistics.median(r[1] for r in runs[m]) for m in ("off", "refs"))
        ratio, delta = t_refs[0] / t_off[0], m_refs - m_off
        ok = ratio <= TIME_LIMIT and delta <= RSS_LIMIT_MB
        failed += 0 if ok else 1
        cells = [f"{t[0]:>7.1f} ({t[1]:.1f}, {t[2]:.1f})".ljust(23) for t in (t_off, t_refs)]
        print(f"{Path(name).stem:<17}{n:>2} {cells[0]} {cells[1]} {ratio:>6.3f} "
              f"{m_off:>7.1f} {m_refs:>8.1f} {delta:>9.2f}  {'PASS' if ok else 'FAIL'}")
    return failed


def measure(args: argparse.Namespace) -> Runs:
    designs = sorted((Path(d) for d in args.designs), key=lambda p: (p.stat().st_size, str(p)))
    techlib = Path(args.techlib or os.environ.get("ODIN3_TECHLIB") or DEFAULT_TECHLIB)
    env = (techlib, args.lock or os.environ.get("ODIN3_HEAVY_LOCK") or None)
    raw: Runs = {}
    for design in designs:
        runs: dict[str, list[tuple[float, float]]] = {"off": [], "refs": []}
        for _ in range(args.repeats):
            for mode in ("off", "refs"):
                runs[mode].append(one_run(args.odin3, design, mode, env))
        raw[design.name] = runs
    return raw


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--odin3")
    ap.add_argument("--repeats", type=int, default=7)
    ap.add_argument("--json")
    ap.add_argument("--techlib")
    ap.add_argument("--lock")
    ap.add_argument("--from-json")
    ap.add_argument("designs", nargs="*")
    args = ap.parse_args(argv)
    if args.from_json:
        raw: Runs = json.loads(Path(args.from_json).read_text())  # the file's key order
    elif args.odin3 and args.designs:
        raw = measure(args)
        if args.json:
            Path(args.json).write_text(json.dumps(raw, indent=1) + "\n")
    else:
        ap.error("give --from-json, or --odin3 and designs")
    return 1 if report(raw) else 0


if __name__ == "__main__":
    sys.exit(main())
