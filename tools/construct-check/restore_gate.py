#!/usr/bin/env python3
"""restore-gate: the cost of restoring a version 2 name map (V1.2.0 Task 9, section 4.5).

Usage::

    restore_gate.py --odin3 O --design D.v --work DIR [--repeats N] [--techlib FILE] [--json OUT]

Setup (not measured): the VTR flow to ``lower`` (no ABC) and
``write_blif --vtr --name-style hash --prov-map`` of DESIGN into DIR. Then N times (default 5),
alternating, ``read_blif`` of that BLIF without a map and with ``--name-map`` (the version 2 map):
the time of a run is the pass time odin3 logs (``pass read_blif: X ms``), the peak RSS is
``Maximum resident set size`` of ``/usr/bin/time -v``. Prints the medians, the ratio and the
delta in MB, the sizes of the BLIF and the map, and the lines the restore logged. Run it under
``tools/heavy-run`` (it takes no lock itself). Exit 0 when every run succeeded.
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

DEFAULT_TECHLIB = Path(__file__).resolve().parents[2] / "lib" / "vtr.o3lib"
PASS_RE = re.compile(r"pass read_blif: ([0-9.]+) ms")
RSS_RE = re.compile(r"Maximum resident set size \(kbytes\): (\d+)")
RESTORE_RE = re.compile(r"name map restore: ([^\n]*)")
KB_PER_MB = 1024.0


def run_timed(odin3: str, script: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["/usr/bin/time", "-v", odin3, "-p", script],
        capture_output=True,
        text=True,
        check=False,
    )


def setup(odin3: str, design: Path, techlib: Path, blif: Path) -> None:
    script = (
        f"read_techlib {techlib}; read_verilog --nolatches --vtr-compat --prov-ast refs {design}; "
        f"proc; opt; flatten; opt -full; wreduce; hardmap; lower; "
        f"write_blif --vtr --name-style hash --prov-map {blif}"
    )
    proc = run_timed(odin3, script)
    if proc.returncode != 0:
        raise SystemExit(f"setup failed:\n{proc.stderr[-800:]}")


def measure(odin3: str, script: str) -> tuple[float, float, str]:
    proc = run_timed(odin3, script)
    took, rss = PASS_RE.search(proc.stderr), RSS_RE.search(proc.stderr)
    if proc.returncode != 0 or took is None or rss is None:
        raise SystemExit(f"{script}: exit {proc.returncode}\n{proc.stderr[-800:]}")
    restore = RESTORE_RE.search(proc.stderr)
    return float(took.group(1)), int(rss.group(1)) / KB_PER_MB, restore.group(1) if restore else ""


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--odin3", required=True)
    ap.add_argument("--design", required=True, type=Path)
    ap.add_argument("--work", required=True, type=Path)
    ap.add_argument("--repeats", type=int, default=5)
    ap.add_argument("--techlib", type=Path, default=DEFAULT_TECHLIB)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)
    args.work.mkdir(parents=True, exist_ok=True)
    blif = args.work / f"{args.design.stem}.blif"
    setup(args.odin3, args.design, args.techlib, blif)
    names = Path(f"{blif}.names")
    base = f"read_techlib {args.techlib}; read_blif "
    scripts = {"plain": f"{base}{blif}", "map": f"{base}--name-map {names} {blif}"}
    runs: dict[str, list[tuple[float, float]]] = {"plain": [], "map": []}
    note = ""
    for _ in range(args.repeats):
        for mode in ("plain", "map"):
            ms, mb, restore = measure(args.odin3, scripts[mode])
            runs[mode].append((ms, mb))
            note = restore or note
    med = {
        m: (statistics.median(r[0] for r in v), statistics.median(r[1] for r in v))
        for m, v in runs.items()
    }
    print(
        f"{args.design.stem}: BLIF {blif.stat().st_size} bytes, map {names.stat().st_size} bytes, "
        f"n = {args.repeats}"
    )
    print(f"  read_blif, no map : {med['plain'][0]:10.1f} ms  {med['plain'][1]:8.1f} MB")
    print(f"  read_blif, map    : {med['map'][0]:10.1f} ms  {med['map'][1]:8.1f} MB")
    print(
        f"  ratio {med['map'][0] / med['plain'][0]:.2f}, "
        f"delta {med['map'][1] - med['plain'][1]:.1f} MB"
    )
    print(f"  restore: {note}")
    if args.json:
        args.json.write_text(json.dumps({"design": args.design.stem, "runs": runs}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
