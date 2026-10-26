#!/usr/bin/env python3
"""export-gate: size, time and peak memory of the version 2 name map (V1.2.0 Task 5, section 5.3).

Usage::

    export_gate.py --odin3 O [--repeats N] [--json OUT] [--techlib FILE] [--lock FILE]
                   [--workdir DIR] <design.v>...
    export_gate.py --from-json RAW.json

For each design, smallest first, N times (default 3), alternating the two ends of one flow
(``v1`` and ``v2``): ``read_techlib TECHLIB; read_verilog --nolatches --vtr-compat --prov-ast refs
F; proc; fsm; opt; flatten; opt -full; wreduce; hardmap; lower; abc --lut 6; write_blif --vtr
--name-style hash [--prov-map] OUT`` (``--prov-map`` for ``v2``).  Each process runs under
``flock -x LOCK`` (``--lock``, else ``ODIN3_HEAVY_LOCK``, else none) and ``/usr/bin/time -v``.
The export time is the pass time odin3 logs for ``write_blif``; the peak RSS is ``Maximum resident
set size``.  The files go to ``--workdir`` (a fresh directory; required with a run) and stay there.
The table gives, per design, n, the number of objects (O and N lines) and R, C lines and bytes of
the version 2 map, the bytes of the version 1 map, the export time (median) of each end, and the
peak RSS (median) of each end.  ``--json`` keeps every raw run; ``--from-json`` prints the table of
a raw file with no run.
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
FLOW = (
    "read_techlib {tech}; read_verilog --nolatches --vtr-compat --prov-ast refs {src}; proc; "
    "fsm; opt; flatten; opt -full; wreduce; hardmap; lower; abc --lut 6; "
    "write_blif --vtr --name-style hash {extra} {out}"
)
PASS_RE = re.compile(r"pass write_blif: ([0-9.]+) ms")
RSS_RE = re.compile(r"Maximum resident set size \(kbytes\): (\d+)")
WALL_RE = re.compile(r"Elapsed \(wall clock\) time .*: (?:(\d+):)?(\d+):([0-9.]+)")

Raw = dict[str, dict[str, list[dict[str, float]]]]


def wall_seconds(text: str) -> float:
    """The wall time /usr/bin/time prints (h:mm:ss or m:ss.ss)."""
    found = WALL_RE.search(text)
    if found is None:
        return 0.0
    hours, minutes, seconds = found.groups()
    return int(hours or 0) * 3600 + int(minutes) * 60 + float(seconds)


def map_shape(path: Path) -> dict[str, float]:
    """Bytes and line counts by type of a map file."""
    counts = {"bytes": float(path.stat().st_size), "O": 0.0, "N": 0.0, "R": 0.0, "C": 0.0}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        kind = line[:2]
        if kind in ("O\t", "N\t", "R\t", "C\t"):
            counts[kind[0]] += 1
    counts["lines"] = counts["O"] + counts["N"] + counts["R"] + counts["C"]
    return counts


def one_run(
    odin3: str, design: Path, end: str, env: tuple[Path, str | None, Path]
) -> dict[str, float]:
    """One flow to one end (v1 or v2): export ms, peak RSS MB, wall s, and the map's shape."""
    tech, lock_file, workdir = env
    out = workdir / f"{design.stem}-{end}.blif"
    extra = "--prov-map" if end == "v2" else ""
    script = FLOW.format(tech=tech, src=design, extra=extra, out=out)
    lock = ["flock", "-x", lock_file] if lock_file else []
    proc = subprocess.run(
        [*lock, "/usr/bin/time", "-v", odin3, "-p", script],
        capture_output=True, text=True, check=False)
    took, rss = PASS_RE.search(proc.stderr), RSS_RE.search(proc.stderr)
    if proc.returncode != 0 or took is None or rss is None:
        raise SystemExit(f"{design.name} {end}: exit {proc.returncode}\n{proc.stderr[-600:]}")
    shape = map_shape(Path(str(out) + ".names"))
    return {"ms": float(took.group(1)), "mb": int(rss.group(1)) / 1024.0,
            "wall": wall_seconds(proc.stderr), **shape}


def med(runs: list[dict[str, float]], key: str) -> float:
    return statistics.median(r[key] for r in runs)


def report(raw: Raw) -> None:
    """Prints the table of the raw runs."""
    print("design            n  objects  R lines  C lines  v2 map bytes  v1 map bytes  "
          "export v1 ms  export v2 ms  RSS v1 MB  RSS v2 MB  delta MB")
    for name, ends in raw.items():
        v1, v2 = ends["v1"], ends["v2"]
        objects = med(v2, "O") + med(v2, "N")
        print(f"{Path(name).stem:<17}{len(v2):>2} {objects:>8.0f} {med(v2, 'R'):>8.0f} "
              f"{med(v2, 'C'):>8.0f} {med(v2, 'bytes'):>13.0f} {med(v1, 'bytes'):>13.0f} "
              f"{med(v1, 'ms'):>13.1f} {med(v2, 'ms'):>13.1f} {med(v1, 'mb'):>10.1f} "
              f"{med(v2, 'mb'):>10.1f} {med(v2, 'mb') - med(v1, 'mb'):>9.1f}")


def measure(args: argparse.Namespace) -> Raw:
    designs = sorted((Path(d) for d in args.designs), key=lambda p: (p.stat().st_size, str(p)))
    techlib = Path(args.techlib or os.environ.get("ODIN3_TECHLIB") or DEFAULT_TECHLIB)
    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    env = (techlib, args.lock or os.environ.get("ODIN3_HEAVY_LOCK") or None, workdir)
    raw: Raw = {}
    for design in designs:
        ends: dict[str, list[dict[str, float]]] = {"v1": [], "v2": []}
        for _ in range(args.repeats):
            for end in ("v1", "v2"):
                ends[end].append(one_run(args.odin3, design, end, env))
        raw[design.name] = ends
    return raw


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--odin3")
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--json")
    ap.add_argument("--techlib")
    ap.add_argument("--lock")
    ap.add_argument("--workdir")
    ap.add_argument("--from-json")
    ap.add_argument("designs", nargs="*")
    args = ap.parse_args(argv)
    if args.from_json:
        raw: Raw = json.loads(Path(args.from_json).read_text())
    elif args.odin3 and args.designs and args.workdir:
        raw = measure(args)
        if args.json:
            Path(args.json).write_text(json.dumps(raw, indent=1) + "\n")
    else:
        ap.error("give --from-json, or --odin3, --workdir and designs")
    report(raw)
    return 0


if __name__ == "__main__":
    sys.exit(main())
