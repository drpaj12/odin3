"""bench-report: the `bench_elab` line, the §15 targets and the smoke check (2D spec §15, plan
Task 27).

``elab-sweep --bench B --bench-report elab`` collects the line each ``B --one FILE`` prints and
calls :func:`report` after the sweep; the stand-alone form checks that a bench prints every column::

    bench-report smoke --bench B [--techlib T] [--mem-cap C] FILE...

``smoke`` runs ``B --one FILE`` on each file under memcap and fails (exit 1) when a run fails or a
line lacks a column of :data:`ELAB_COLUMNS`. Exit codes: 0 pass; 1 a failure; 2 usage; 77 skipped
(no memory cap possible).

A line is ``key=value`` words (:func:`parse_line`). The profile ``elab`` is §15's table; the profile
``proc`` is 2E's §19 table, whose targets function and report live in
``tools/proc-check/bench_proc_report.py`` (loaded on first use) and which also takes the
expect-fail list's paths, so ``elab-sweep --bench bench_proc --bench-report proc`` reads the same
rows ``bench-proc-report report`` does.
"""

from __future__ import annotations

import argparse
import importlib.util
import shutil
import sys
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from types import ModuleType
from typing import NamedTuple

import memcap

TOOL = "bench-report"
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_SKIP = 0, 1, 2, 77

# The columns tests/bench/bench_elab.c prints, in its order.
ELAB_COLUMNS = (
    "file", "bytes", "parse_ms", "stage1_ms", "bind_ms", "stage2_ms", "elab_ms",
    "parsed_nodes", "parsed_bytes", "elab_nodes", "elab_bytes",
    "modules", "specs", "cells", "procs", "mems", "nodes", "nets", "wires", "pins",
    "est_pins_B", "est_nets_B", "est_nodes_B", "est_wires_B", "est_bodies_B", "est_elab_B",
    "est_total_B", "est_stage1_B", "ir_B", "rss_kib",
)  # fmt: skip
ESTIMATE_TERMS = ("est_pins_B", "est_nets_B", "est_nodes_B", "est_wires_B", "est_bodies_B",
                  "est_elab_B", "est_total_B", "est_stage1_B", "ir_B")

# §15's targets. "MB" and "GB" are decimal (10^6 and 10^9 bytes): the stricter reading of
# ru_maxrss (KiB), so a borderline run is a miss, not a pass.
MCML = "mcml.v"
LENET = "koios/lenet.v"
MCML_MS = 500.0
MCML_RSS_BYTES = 200 * 10**6
LENET_MS = 10000.0
LENET_RSS_BYTES = 2 * 10**9
SET_FILES = 31
SET_MS = 3000.0
KIB = 1024

Row = Mapping[str, str]


class Target(NamedTuple):
    name: str
    verdict: str  # ok, MISSED or n/a
    detail: str


def parse_line(text: str) -> dict[str, str]:
    """``key=value`` words of one bench line."""
    out: dict[str, str] = {}
    for word in text.split():
        key, eq, value = word.partition("=")
        if not eq or not key:
            raise ValueError(f"bench line: {word!r} is not key=value")
        out[key] = value
    return out


def missing_columns(row: Row, columns: Sequence[str] = ELAB_COLUMNS) -> list[str]:
    return [name for name in columns if name not in row]


def _number(row: Row, key: str) -> float:
    return float(row[key])


def _judge(name: str, value: float, limit: float, unit: str, scale: float = 1.0) -> Target:
    shown = value / scale
    ok = value <= limit
    return Target(name, "ok" if ok else "MISSED",
                  f"{shown:.3f} {unit} (target <= {limit / scale:g} {unit})")


def _na(name: str, why: str) -> Target:
    return Target(name, "n/a", why)


def _one_file(rows: Mapping[str, Row], rel: str, label: str, ms: float, rss: int) -> list[Target]:
    row = rows.get(rel)
    if row is None:
        return [_na(f"{label} stage 1 + stage 2", f"{rel} is not in this run"),
                _na(f"{label} peak RSS", f"{rel} is not in this run")]
    return [
        _judge(f"{label} stage 1 + stage 2", _number(row, "elab_ms"), ms, "s", 1000.0),
        _judge(f"{label} peak RSS", _number(row, "rss_kib") * KIB, rss, "MB", 1e6),
    ]


def _set_target(rows: Mapping[str, Row]) -> Target:
    name = f"{SET_FILES} verilog/*.v files, total elaboration"
    files = {rel: row for rel, row in rows.items() if "/" not in rel}
    if len(files) != SET_FILES:
        return _na(name, f"{len(files)} of the {SET_FILES} files are in this run")
    return _judge(name, sum(_number(row, "elab_ms") for row in files.values()), SET_MS, "s",
                  1000.0)


def elab_targets(rows: Mapping[str, Row]) -> list[Target]:
    """§15's table over the rows (relative path to the line's columns)."""
    return [
        *_one_file(rows, MCML, "mcml.v", MCML_MS, MCML_RSS_BYTES),
        *_one_file(rows, LENET, "lenet.v", LENET_MS, LENET_RSS_BYTES),
        _set_target(rows),
    ]


def _terms(row: Row) -> str:
    return " ".join(f"{key}={row[key]}" for key in ESTIMATE_TERMS if key in row)


_PROC_MODULE: list[ModuleType] = []


def proc_module() -> ModuleType:
    """2E's ``bench_proc_report`` (``tools/proc-check``), loaded once; it imports only memcap."""
    if not _PROC_MODULE and "bench_proc_report" in sys.modules:
        _PROC_MODULE.append(sys.modules["bench_proc_report"])  # already imported by name
    if not _PROC_MODULE:
        path = Path(__file__).resolve().parent.parent / "proc-check" / "bench_proc_report.py"
        spec = importlib.util.spec_from_file_location("bench_proc_report", path)
        if spec is None or spec.loader is None:
            raise ImportError(f"cannot load {path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules["bench_proc_report"] = module
        spec.loader.exec_module(module)
        _PROC_MODULE.append(module)
    return _PROC_MODULE[0]


def proc_targets(rows: Mapping[str, Row], expected: frozenset[str] = frozenset()) -> list[Target]:
    """2E's §19 table (profile ``proc``) over the rows; ``expected`` are the expect-fail paths."""
    return [Target(*t) for t in proc_module().proc_targets(rows, expected)]


PROFILES: dict[str, Callable[..., list[Target]]] = {"elab": elab_targets, "proc": proc_targets}


def report(profile: str, rows: Mapping[str, Row],
           expected: frozenset[str] = frozenset()) -> tuple[list[str], bool]:
    """(lines to print, whether a target was missed). A miss is followed by the estimate terms
    of the files it names: it is investigated and reported, never relaxed. ``expected`` (the
    expect-fail paths) matters to profile ``proc`` only."""
    if profile not in PROFILES:
        raise ValueError(f"unknown bench profile {profile!r} (known: {', '.join(PROFILES)})")
    if profile == "proc":
        done: tuple[list[str], bool] = proc_module().report(profile, rows, expected)
        return done
    targets = PROFILES[profile](rows)
    lines = ["bench targets (spec 15):"]
    lines += [f"  {t.verdict:<6} {t.name}: {t.detail}" for t in targets]
    missed = any(t.verdict == "MISSED" for t in targets)
    for rel in (MCML, LENET):
        if rel in rows and (missed or rel == LENET):
            lines.append(f"  estimate terms of {rel}: {_terms(rows[rel])}")
    return lines, missed


def smoke(bench: str, files: Sequence[str], techlib: str | None, cap: int) -> int:
    """Run ``bench --one FILE`` on each file; every column must be present."""
    failed = 0
    for path in files:
        argv = [bench, "--one", path, *(["--techlib", techlib] if techlib else [])]
        done = memcap.run(argv, cap, capture_output=True, timeout=600)
        if done.returncode != 0:
            print(f"{TOOL}: {path}: exit {done.returncode}: {(done.stderr or '').strip()}",
                  file=sys.stderr)
            failed += 1
            continue
        try:
            absent = missing_columns(parse_line(done.stdout))
        except ValueError as exc:
            print(f"{TOOL}: {path}: {exc}", file=sys.stderr)
            failed += 1
            continue
        if absent:
            print(f"{TOOL}: {path}: missing columns: {', '.join(absent)}", file=sys.stderr)
            failed += 1
    if not failed:
        print(f"{TOOL}: smoke: {len(files)} files, every one of the {len(ELAB_COLUMNS)} columns")
    return EXIT_FAIL if failed else EXIT_OK


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog=TOOL)
    sub = ap.add_subparsers(dest="command", required=True)
    sm = sub.add_parser("smoke", help="check that a bench prints every column")
    sm.add_argument("--bench", required=True)
    sm.add_argument("--techlib")
    sm.add_argument("--mem-cap", default="2G")
    sm.add_argument("files", nargs="+")
    args = ap.parse_args(sys.argv[1:] if argv is None else argv)
    from elab_sweep import parse_size  # late: elab_sweep imports this module

    try:
        cap = parse_size(args.mem_cap)
        memcap.check_cap(cap)
        bench = shutil.which(args.bench) or args.bench
        memcap.wrap([bench], cap)
        return smoke(bench, args.files, args.techlib, cap)
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP
    except (ValueError, OSError) as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())
