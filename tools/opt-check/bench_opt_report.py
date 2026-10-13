"""bench-opt-report: the `bench_opt` line, the 2F spec §19 targets and the smoke check (plan
Task 19).

``elab-sweep --bench build/release/bench_opt`` runs ``bench_opt --one FILE`` per file, smallest
first, and prints the line each prints after the file's verdict as ``bench: <line>``. Two forms::

    bench-opt-report report [--expect-fail F]... SWEEP_LOG   # the §19 targets from a sweep log
    bench-opt-report smoke --bench B [--techlib T] [--mem-cap C] FILE...

``report`` prints the targets (:func:`report`); a MISSED target makes it exit 1, followed by the
per-sub-pass times and the memory columns of the files it names: a miss is investigated and
reported, never relaxed. A file or set the run lacks, or a row short of a column, is MISSED unless
``--expect-fail F`` (the sweep's list) names the file, so a partial run never passes. ``smoke`` runs
``B --one FILE`` on each file under memcap and fails (exit 1) when a run fails or a line lacks a
column of :data:`OPT_COLUMNS`. Exit codes: 0 pass; 1 a failure or a missed target; 2 usage; 77
skipped (no memory cap possible).

The expect-fail reader, the size parser and the target type are 2E's
(``tools/proc-check/bench_proc_report.py``). Times are seconds (the bench's ``t_*`` columns).
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path

import memcap
from bench_proc_report import (
    EXIT_FAIL,
    EXIT_OK,
    EXIT_SKIP,
    EXIT_USAGE,
    Target,
    _judge,
    _parse_size,
    expect_paths,
)

TOOL = "bench-opt-report"

# The columns tests/bench/bench_opt.c prints that the targets and the miss report read, plus the
# row's identity; the sub-pass and rule-counter columns beyond these vary with the design.
OPT_COLUMNS = (
    "file", "cells0", "nets0", "pins0", "cellsp", "netsp", "pinsp", "cells1", "nets1", "pins1",
    "iters", "t_total", "t_expr", "t_merge", "t_muxtree", "t_reduce", "t_share", "t_dff",
    "t_clean", "t_setup", "setup_rounds", "t_compact", "dead_nodes", "dead_nets", "dead_wires",
    "full_iters", "full_t_total", "full_t_expr", "full_t_merge", "full_t_muxtree",
    "full_t_reduce", "full_t_share", "full_t_dff", "full_t_clean", "full_t_setup",
    "rss_proc_kib", "rss_proc_cur_kib", "rss_opt_kib", "rss_opt_growth_pct", "rss_compact_kib",
    "rss_full_kib", "rss_peak_kib", "rss_src",
)  # fmt: skip
TIME_COLUMNS = ("iters", "t_total", "t_expr", "t_setup", "t_merge", "t_muxtree", "t_reduce",
                "t_share", "t_dff", "t_clean", "t_compact", "full_iters", "full_t_total")
MEMORY_COLUMNS = ("rss_proc_cur_kib", "rss_opt_kib", "rss_opt_growth_pct", "rss_compact_kib",
                  "rss_full_kib", "dead_nodes", "dead_nets", "rss_src")

# §19's targets (seconds; percent).
MCML = "mcml.v"
LENET = "koios/lenet.v"
MCML_OPT_S = 1.0
MCML_FULL_S = 0.3
LENET_OPT_S = 15.0
LENET_RSS_GROWTH_PCT = 25.0
VTR_FILES = 31
VTR_OPT_S = 3.0

Row = Mapping[str, str]


def parse_line(text: str) -> dict[str, str]:
    """``key=value`` words of one bench line, after its leading ``bench_opt`` word."""
    words = text.split()
    if words and words[0] == "bench_opt":
        words = words[1:]
    out: dict[str, str] = {}
    for word in words:
        key, eq, value = word.partition("=")
        if not eq or not key:
            raise ValueError(f"bench line: {word!r} is not key=value")
        out[key] = value
    return out


VERDICT = re.compile(r"^(?:ok|xfail|FAIL|XPASS|crash|skip)\s+(\S+) \(")


def parse_sweep_log(text: str) -> dict[str, dict[str, str]]:
    """The rows of an elab-sweep ``--bench`` log: each ``bench: <line>`` belongs to the verdict
    line before it, which names the file's relative path."""
    rows: dict[str, dict[str, str]] = {}
    rel: str | None = None
    for line in text.splitlines():
        match = VERDICT.match(line)
        if match:
            rel = match.group(1)
        elif line.startswith("bench: ") and rel is not None:
            rows[rel] = parse_line(line[len("bench: "):])
    return rows


def missing_columns(row: Row, columns: Sequence[str] = OPT_COLUMNS) -> list[str]:
    return [name for name in columns if name not in row]


def _absent(name: str, rel: str, expected: frozenset[str]) -> Target:
    """A design the run lacks: a miss, unless the expect-fail list names it (then n/a)."""
    return Target(name, "n/a" if rel in expected else "MISSED", f"{rel} is not in this run")


def _need_hwm(name: str, row: Row) -> Target | None:
    """The RSS target is a high-water-mark reading; the fallback is a lower bound, never a pass."""
    if row.get("rss_src") != "hwm":
        return Target(name, "MISSED", f"rss_src={row.get('rss_src')}: not a high-water mark")
    return None


def opt_targets(rows: Mapping[str, Row],
                expected: frozenset[str] = frozenset()) -> list[Target]:
    """§19's table over the rows (relative path to the line's columns). A file or set the run
    lacks is MISSED unless ``expected`` (the expect-fail list's paths) accounts for it."""
    out: list[Target] = []
    mcml = rows.get(MCML)
    names = ("mcml.v, Phase 2 opt passes", "mcml.v, opt -full")
    if mcml is None:
        out += [_absent(name, MCML, expected) for name in names]
    else:
        out.append(_judge(names[0], float(mcml["t_total"]), MCML_OPT_S, "s"))
        out.append(_judge(names[1], float(mcml["full_t_total"]), MCML_FULL_S, "s"))
    lenet = rows.get(LENET)
    names = ("lenet.v, Phase 2 opt passes", "lenet.v, peak RSS growth over the post-proc IR")
    if lenet is None:
        out += [_absent(name, LENET, expected) for name in names]
    else:
        out.append(_judge(names[0], float(lenet["t_total"]), LENET_OPT_S, "s"))
        out.append(_need_hwm(names[1], lenet) or _judge(
            names[1], float(lenet["rss_opt_growth_pct"]), LENET_RSS_GROWTH_PCT, "%"))
    vtr = {rel: row for rel, row in rows.items() if "/" not in rel}
    name = f"the {VTR_FILES} VTR files, total Phase 2 opt passes"
    need = VTR_FILES - sum("/" not in rel for rel in expected)
    if len(vtr) != need:
        out.append(Target(name, "MISSED", f"{len(vtr)} of the {need} files are timed "
                                          f"({VTR_FILES} less the expected failures)"))
    else:
        out.append(_judge(name, sum(float(row["t_total"]) for row in vtr.values()), VTR_OPT_S,
                          "s"))
    return out


def _fields(row: Row, keys: Sequence[str]) -> str:
    return " ".join(f"{key}={row[key]}" for key in keys if key in row)


def report(rows: Mapping[str, Row],
           expected: frozenset[str] = frozenset()) -> tuple[list[str], bool]:
    """(lines to print, whether a target was missed). The sub-pass times and memory columns of
    the files a target names follow a miss (always for lenet.v): investigated, never relaxed."""
    short = {rel: missing_columns(row) for rel, row in rows.items() if missing_columns(row)}
    whole = {rel: row for rel, row in rows.items() if rel not in short}
    targets = opt_targets(whole, expected)
    targets += [Target(f"{rel} row", "MISSED", f"missing columns: {', '.join(gone)}")
                for rel, gone in short.items()]
    lines = ["bench targets (spec 19):"]
    lines += [f"  {t.verdict:<6} {t.name}: {t.detail}" for t in targets]
    missed = any(t.verdict == "MISSED" for t in targets)
    for rel in (MCML, LENET):
        if rel in rows and (missed or rel == LENET):
            lines.append(f"  times of {rel}: {_fields(rows[rel], TIME_COLUMNS)}")
            lines.append(f"  memory of {rel}: {_fields(rows[rel], MEMORY_COLUMNS)}")
    slowest = max(whole.items(), key=lambda kv: float(kv[1]["t_total"]), default=None)
    if slowest is not None:
        lines.append(f"  slowest file: {slowest[0]} {float(slowest[1]['t_total']):.3f} s")
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
        print(f"{TOOL}: smoke: {len(files)} files, every one of the {len(OPT_COLUMNS)} columns")
    return EXIT_FAIL if failed else EXIT_OK


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog=TOOL)
    sub = ap.add_subparsers(dest="command", required=True)
    sm = sub.add_parser("smoke", help="check that a bench prints every column")
    sm.add_argument("--bench", required=True)
    sm.add_argument("--techlib")
    sm.add_argument("--mem-cap", default="3G")
    sm.add_argument("files", nargs="+")
    rp = sub.add_parser("report", help="the §19 targets from an elab-sweep --bench log")
    rp.add_argument("log", help="the sweep's output (a file, or - for stdin)")
    rp.add_argument("--expect-fail", action="append", default=[], metavar="F",
                    help="the sweep's expect-fail list: its files may be absent from the run")
    args = ap.parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.command == "report":
            text = sys.stdin.read() if args.log == "-" else Path(args.log).read_text("utf-8")
            lines, missed = report(parse_sweep_log(text), expect_paths(args.expect_fail))
            print("\n".join(lines))
            return EXIT_FAIL if missed else EXIT_OK
        cap = _parse_size(args.mem_cap)
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
