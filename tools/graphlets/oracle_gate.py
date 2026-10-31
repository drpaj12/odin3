#!/usr/bin/env python3
"""Exactness gate of the undirected graphlet counter against the ORCA binary (external test oracle
only; spec docs/specs/2026-10-30-graphlets-design.md section 9) and the runtime/memory table.

Usage: oracle_gate.py --odin3 ODIN3 --lib vtr.o3lib --glet GLET_COUNT --orca ORCA --out DIR BLIF...
For each BLIF: odin3 reads it and writes the graph (`graphlets --exclude-global --edges-out`), then
the 4- and 5-node orbit counts of GLET_COUNT and of ORCA on the undirected simple graph must be
identical. Prints one table row per design (nodes, edges, seconds and peak MiB of both tools).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path


def timed(cmd: list[str]) -> tuple[bool, float, float]:
    """(ok, seconds, peak MiB) of one command (wait4 gives the child's own resource usage)."""
    start = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _, status, usage = os.wait4(proc.pid, 0)
    proc.returncode = 0  # reaped by wait4
    return status == 0, time.monotonic() - start, usage.ru_maxrss / 1024.0


def undirected(src: Path, dst: Path) -> tuple[int, int]:
    lines = src.read_text().split("\n")
    n = int(lines[0].split()[0])
    edges = set()
    for ln in lines[1:]:
        if ln.strip():
            a, b = (int(x) for x in ln.split())
            edges.add((min(a, b), max(a, b)))
    dst.write_text(f"{n} {len(edges)}\n" + "".join(f"{a} {b}\n" for a, b in sorted(edges)))
    return n, len(edges)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--odin3", required=True)
    ap.add_argument("--lib", required=True)
    ap.add_argument("--glet", required=True)
    ap.add_argument("--orca", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("blifs", nargs="+")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    blifs = sorted(args.blifs, key=lambda p: Path(p).stat().st_size)  # smallest first
    passed = total = skipped = 0
    print("design,nodes,edges,size,mine_s,mine_MiB,orca_s,orca_MiB,identical")
    for blif in blifs:
        name = Path(blif).name.removesuffix(".blif")
        directed_edges = out / f"{name}.dir.txt"
        graph = out / f"{name}.und.txt"
        script = (
            f"read_techlib {args.lib}; read_blif {blif}; "
            f"graphlets --exclude-global --edges-out {directed_edges}"
        )
        done = subprocess.run([args.odin3, "-p", script], capture_output=True, text=True)
        if done.returncode != 0 or not directed_edges.exists():
            skipped += 1
            print(f"{name},skipped,odin3 could not read it", file=sys.stderr)
            continue
        n, m = undirected(directed_edges, graph)
        for size in (4, 5):
            mine, theirs = out / f"{name}.mine{size}", out / f"{name}.orca{size}"
            ok1, t1, m1 = timed([args.glet, str(size), str(graph), str(mine)])
            ok2, t2, m2 = timed([args.orca, "node", str(size), str(graph), str(theirs)])
            same = ok1 and ok2 and mine.read_text().split() == theirs.read_text().split()
            total += 1
            passed += int(same)
            print(f"{name},{n},{m},{size},{t1:.2f},{m1:.0f},{t2:.2f},{m2:.0f},{same}")
    print(f"gate: {passed}/{total} identical, {skipped} designs skipped", file=sys.stderr)
    return 0 if passed == total and total > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
