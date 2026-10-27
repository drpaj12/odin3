#!/usr/bin/env python3
"""export-gate: size, time and peak memory of the version 2 name map (V1.2.0 Task 5, section 5.3).

Usage::

    export_gate.py --odin3 O [--repeats N] [--single] [--json OUT] [--jsonl OUT]
                   [--techlib FILE] [--lock FILE] [--workdir DIR] <design.v>...
    export_gate.py --from-json RAW.json
    export_gate.py --from-jsonl RUNS.jsonl

``--single`` runs the flow once per design (ABC dominates the wall time) and then writes each end
N times in the same ``-p`` script; the export times are the per-write pass timers and the peak RSS
and VPR samples are those of that one invocation.  The rows of the 2N writes are appended to
``--jsonl`` when that invocation ends (one process, so its writes end together); a stop before
that loses the invocation, not part of it.  Without it, for each design, smallest first, N
times (default 3), alternating the two ends of one flow
(``v1`` and ``v2``): ``read_techlib TECHLIB; read_verilog --nolatches --vtr-compat --prov-ast refs
F; proc; fsm; opt; flatten; opt -full; wreduce; hardmap; lower; abc --lut 6; write_blif --vtr
--name-style hash [--prov-map] OUT`` (``--prov-map`` for ``v2``).  Each process runs under
an exclusive ``flock`` on LOCK (``--lock``, else ``ODIN3_HEAVY_LOCK``, else none), taken by this
script and held until the process has ended, and ``/usr/bin/time -v``.  While it runs, a thread of
this script counts ``vpr`` processes (``pgrep -c vpr``) at once and then every 5 s inside the lock,
and once more before the lock is released; the table says in how many samples a ``vpr`` process
was seen.
The export time is the pass time odin3 logs for ``write_blif``; the peak RSS is ``Maximum resident
set size``.  The files go to ``--workdir`` (a fresh directory; required with a run) and stay there.
The table gives, per design, n, the number of objects (O and N lines) and R, C lines and bytes of
the version 2 map, the bytes of the version 1 map, the export time (median) of each end, and the
peak RSS (median) of each end.  ``--json`` keeps every raw run (written at the end); ``--jsonl``
appends each finished run as one JSON line at once, so a stop loses nothing; ``--from-json`` and
``--from-jsonl`` print the table of a raw file with no run.
"""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import re
import statistics
import subprocess
import sys
import threading
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
    proc, samples = run_locked(odin3, script, lock_file)
    took, rss = PASS_RE.search(proc.stderr), RSS_RE.search(proc.stderr)
    if proc.returncode != 0 or took is None or rss is None:
        raise SystemExit(f"{design.name} {end}: exit {proc.returncode}\n{proc.stderr[-600:]}")
    shape = map_shape(Path(str(out) + ".names"))
    return {"ms": float(took.group(1)), "mb": int(rss.group(1)) / 1024.0,
            "wall": wall_seconds(proc.stderr), **shape, **vpr_fields(samples)}


VPR_SAMPLE_S = 5.0


def vpr_count() -> int:
    """The number of running processes whose name matches ``vpr`` (0 when pgrep finds none)."""
    found = subprocess.run(["pgrep", "-c", "vpr"], capture_output=True, text=True, check=False)
    return int(found.stdout.strip() or 0)


def run_locked(
    odin3: str, script: str, lock_file: str | None
) -> tuple[subprocess.CompletedProcess[str], list[int]]:
    """Runs ``odin3 -p script`` under /usr/bin/time -v, holding an exclusive flock on lock_file.

    A thread of this script samples ``vpr_count()`` when the lock is taken, every VPR_SAMPLE_S
    seconds while the process runs, and once after it ended, all before the lock is released.
    Returns the finished process and the samples.
    """
    fd = os.open(lock_file, os.O_RDWR | os.O_CREAT, 0o666) if lock_file else -1
    try:
        if fd >= 0:
            fcntl.flock(fd, fcntl.LOCK_EX)
        samples: list[int] = [vpr_count()]
        done = threading.Event()

        def sample() -> None:
            while not done.wait(VPR_SAMPLE_S):
                samples.append(vpr_count())

        sampler = threading.Thread(target=sample, daemon=True)
        sampler.start()
        try:
            proc = subprocess.run(
                ["/usr/bin/time", "-v", odin3, "-p", script],
                capture_output=True, text=True, check=False)
        finally:
            done.set()
            sampler.join()
        samples.append(vpr_count())
        return proc, samples
    finally:
        if fd >= 0:
            os.close(fd)  # closing the descriptor releases the flock


def vpr_fields(samples: list[int]) -> dict[str, float]:
    """The run fields for the samples: how many, and in how many a vpr process was seen."""
    return {"vpr_samples": float(len(samples)),
            "vpr_busy": float(sum(1 for n in samples if n > 0))}


def med(runs: list[dict[str, float]], key: str) -> float:
    return statistics.median(r[key] for r in runs)


def vpr_label(runs: list[dict[str, float]]) -> str:
    """The VPR column: samples with a vpr process over all samples, or how an old row was seen."""
    sampled = [r for r in runs if "vpr_samples" in r]
    if sampled:
        seen = int(sum(r["vpr_busy"] for r in sampled))
        return f"vpr seen in {seen}/{int(sum(r['vpr_samples'] for r in sampled))} samples"
    if any(r.get("vpr_start", -1.0) < 0 for r in runs):
        return "not recorded"
    busy = sum(1 for r in runs if r["vpr_start"] > 0 or r.get("vpr_end", 0) > 0)
    return f"vpr seen at the start or end of {busy}/{len(runs)} runs"


def report(raw: Raw) -> None:
    """Prints the table of the raw runs."""
    print("design            n  objects  R lines  C lines  v2 map bytes  v1 map bytes  "
          "export v1 ms  export v2 ms  RSS v1 MB  RSS v2 MB  delta MB  VPR processes")
    for name, ends in raw.items():
        v1, v2 = ends["v1"], ends["v2"]
        objects = med(v2, "O") + med(v2, "N")
        print(f"{Path(name).stem:<17}{len(v2):>2} {objects:>8.0f} {med(v2, 'R'):>8.0f} "
              f"{med(v2, 'C'):>8.0f} {med(v2, 'bytes'):>13.0f} {med(v1, 'bytes'):>13.0f} "
              f"{med(v1, 'ms'):>13.1f} {med(v2, 'ms'):>13.1f} {med(v1, 'mb'):>10.1f} "
              f"{med(v2, 'mb'):>10.1f} {med(v2, 'mb') - med(v1, 'mb'):>9.1f}  "
              f"{vpr_label(v1 + v2)}")


def from_jsonl(path: Path) -> Raw:
    """The raw runs of a JSON-lines file (one run per line: design, end, then the run's numbers)."""
    raw: Raw = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        run = json.loads(line)
        ends = raw.setdefault(run.pop("design"), {"v1": [], "v2": []})
        ends[run.pop("end")].append(run)
    return raw


def append_run(path: Path, design: Path, end: str, run: dict[str, float]) -> None:
    """Appends one finished run to the JSON-lines file and flushes it to disk."""
    with path.open("a", encoding="utf-8") as out:
        out.write(json.dumps({"design": design.name, "end": end, **run}) + "\n")
        out.flush()
        os.fsync(out.fileno())


def one_flow(
    odin3: str, design: Path, repeats: int, env: tuple[Path, str | None, Path]
) -> dict[str, list[dict[str, float]]]:
    """One odin3 invocation: the flow once, then ``repeats`` writes of each end (v1, v2).

    The export time of each write is its own pass timer; the peak RSS and the VPR counts are
    those of the whole invocation (one value, so n counts writes, not flows).
    """
    tech, lock_file, workdir = env
    head = FLOW.split("write_blif", 1)[0].format(tech=tech, src=design)
    writes = []
    for i in range(repeats):
        for end in ("v1", "v2"):
            extra = "--prov-map" if end == "v2" else ""
            out = workdir / f"{design.stem}-{end}-{i}.blif"
            writes.append(f"write_blif --vtr --name-style hash {extra} {out}")
    script = head + "; ".join(writes)
    proc, samples = run_locked(odin3, script, lock_file)
    times, rss = PASS_RE.findall(proc.stderr), RSS_RE.search(proc.stderr)
    if proc.returncode != 0 or len(times) != 2 * repeats or rss is None:
        raise SystemExit(f"{design.name}: exit {proc.returncode}\n{proc.stderr[-600:]}")
    common = {"mb": int(rss.group(1)) / 1024.0, "wall": wall_seconds(proc.stderr)}
    ends: dict[str, list[dict[str, float]]] = {"v1": [], "v2": []}
    for i in range(repeats):
        for k, end in enumerate(("v1", "v2")):
            out = workdir / f"{design.stem}-{end}-{i}.blif"
            shape = map_shape(Path(str(out) + ".names"))
            first = i == 0 and k == 0  # the samples belong to the invocation: count them once
            ends[end].append({"ms": float(times[2 * i + k]), **common, **shape,
                              **(vpr_fields(samples) if first else {})})
    return ends


def measure(args: argparse.Namespace) -> Raw:
    designs = sorted((Path(d) for d in args.designs), key=lambda p: (p.stat().st_size, str(p)))
    techlib = Path(args.techlib or os.environ.get("ODIN3_TECHLIB") or DEFAULT_TECHLIB)
    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    env = (techlib, args.lock or os.environ.get("ODIN3_HEAVY_LOCK") or None, workdir)
    raw: Raw = {}
    for design in designs:
        if args.single:
            ends = one_flow(args.odin3, design, args.repeats, env)
            for end in ("v1", "v2"):
                for run in ends[end]:
                    if args.jsonl:
                        append_run(Path(args.jsonl), design, end, run)
            raw[design.name] = ends
            continue
        ends = {"v1": [], "v2": []}
        for _ in range(args.repeats):
            for end in ("v1", "v2"):
                run = one_run(args.odin3, design, end, env)
                ends[end].append(run)
                if args.jsonl:
                    append_run(Path(args.jsonl), design, end, run)
        raw[design.name] = ends
    return raw


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--odin3")
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--json")
    ap.add_argument("--jsonl")
    ap.add_argument("--single", action="store_true")
    ap.add_argument("--from-jsonl")
    ap.add_argument("--techlib")
    ap.add_argument("--lock")
    ap.add_argument("--workdir")
    ap.add_argument("--from-json")
    ap.add_argument("designs", nargs="*")
    args = ap.parse_args(argv)
    if args.from_jsonl:
        raw: Raw = from_jsonl(Path(args.from_jsonl))
    elif args.from_json:
        raw = json.loads(Path(args.from_json).read_text())
    elif args.odin3 and args.designs and args.workdir:
        raw = measure(args)
        if args.json:
            Path(args.json).write_text(json.dumps(raw, indent=1) + "\n")
    else:
        ap.error("give --from-json, --from-jsonl, or --odin3, --workdir and designs")
    report(raw)
    return 0


if __name__ == "__main__":
    sys.exit(main())
