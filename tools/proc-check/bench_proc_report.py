"""bench-proc-report: the `bench_proc` line, the 2E spec §19 targets and the smoke check (plan
Task 13).

``elab-sweep --bench B --mem-cap 6G`` runs ``B --one FILE`` per file, smallest first, and prints the
line each prints after the file's verdict as ``bench: <line>``. Two forms read that output::

    bench-proc-report report SWEEP_LOG           # the §19 targets from an elab-sweep log
    bench-proc-report smoke --bench B [--techlib T] [--mem-cap C] FILE...

``report`` prints the targets (:func:`report`); a MISSED target makes it exit 1, followed by the
estimate terms and step times of the files it names: a miss is investigated and reported, never
relaxed. A file or set the run lacks, or a row short of a column, is MISSED unless ``--expect-fail
F`` (the sweep's list) names the file, so a partial run never passes. ``smoke`` runs ``B --one FILE`` on each file under memcap and fails (exit 1) when a run
fails or a line lacks a column of :data:`PROC_COLUMNS`. Exit codes: 0 pass; 1 a failure; 2 usage;
77 skipped (no memory cap possible).

The profile mirrors 2D's ``bench_report.py`` (``parse_line``, ``missing_columns``, a targets
function and ``report``): :func:`proc_targets` is registered as profile ``proc`` in its
``PROFILES`` once both are on one branch, so ``elab-sweep --bench-report proc`` reads the same
rows. Rows are keyed by the file's path relative to the benchmark root (elab-sweep's ``Item.rel``).
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import NamedTuple

import memcap

TOOL = "bench-proc-report"
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_SKIP = 0, 1, 2, 77

# The columns tests/bench/bench_proc.c prints, in its order.
PROC_COLUMNS = (
    "file", "bytes", "read_ms", "trio_ms", "trio", "rss_read_kib", "modules", "procs",
    "a_ms", "b_ms", "c_ms", "d_ms", "e_ms", "f_ms", "noopt_ms", "proc_ms", "budget_ms",
    "budget_calls", "warnings",
    "c_mux", "c_pmux", "c_eq", "c_helper", "c_dff", "c_adff", "c_aldff", "c_dffsr", "c_dlatch",
    "nodes_created", "nets_created", "pins_created", "mux_pins",
    "est_mux_pins_B", "est_nets_B", "est_nodes_B", "est_pw_B", "est_total_B", "ir_B", "rss_kib",
)  # fmt: skip
STEP_COLUMNS = ("a_ms", "b_ms", "c_ms", "d_ms", "e_ms", "f_ms", "noopt_ms", "proc_ms",
                "budget_ms", "budget_calls", "modules", "procs")
ESTIMATE_TERMS = ("est_mux_pins_B", "est_nets_B", "est_nodes_B", "est_pw_B", "est_total_B",
                  "ir_B", "rss_read_kib", "rss_kib")

# §19's targets. "MB" and "GB" are decimal (10^6 and 10^9 bytes): the stricter reading of
# ru_maxrss (KiB), so a borderline run is a miss, not a pass.
MCML = "mcml.v"
LENET = "koios/lenet.v"
MCML_MS = 200.0
LENET_MS = 5000.0
LENET_RSS_BYTES = 3 * 10**9
VTR_FILES = 31
VTR_MS = 500.0
KOIOS_FILES = 33
KOIOS_MS = 8000.0
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


def missing_columns(row: Row, columns: Sequence[str] = PROC_COLUMNS) -> list[str]:
    return [name for name in columns if name not in row]


def _number(row: Row, key: str) -> float:
    return float(row[key])


def _judge(name: str, value: float, limit: float, unit: str, scale: float = 1.0) -> Target:
    ok = value <= limit
    return Target(name, "ok" if ok else "MISSED",
                  f"{value / scale:.3f} {unit} (target <= {limit / scale:g} {unit})")


def _absent(name: str, why: str, expected: bool) -> Target:
    """A design the run lacks: a miss, unless the expect-fail list names it (then n/a)."""
    return Target(name, "n/a" if expected else "MISSED", why)


def _one_file(rows: Mapping[str, Row], rel: str, label: str, ms: float, rss: int | None,
              expected: frozenset[str]) -> list[Target]:
    row = rows.get(rel)
    names = [f"{label} proc --noopt"] + ([f"{label}, through proc, peak RSS"] if rss else [])
    if row is None:
        return [_absent(name, f"{rel} is not in this run", rel in expected) for name in names]
    out = [_judge(names[0], _number(row, "noopt_ms"), ms, "s", 1000.0)]
    if rss:
        out.append(_judge(names[1], _number(row, "rss_kib") * KIB, rss, "GB", 1e9))
    return out


def _set_target(name: str, files: Mapping[str, Row], count: int, ms: float,
                listed: int) -> Target:
    """``count`` files less the ``listed`` the expect-fail list names must all be timed."""
    need = count - listed
    if len(files) != need:
        return Target(name, "MISSED", f"{len(files)} of the {need} files are timed "
                      f"({count} less {listed} expected failures)")
    return _judge(name, sum(_number(row, "noopt_ms") for row in files.values()), ms, "s", 1000.0)


def proc_targets(rows: Mapping[str, Row],
                 expected: frozenset[str] = frozenset()) -> list[Target]:
    """§19's table over the rows (relative path to the line's columns). A file or set the run
    lacks is MISSED unless ``expected`` (the expect-fail list's paths) accounts for it."""
    vtr = {rel: row for rel, row in rows.items() if "/" not in rel}
    koios = {rel: row for rel, row in rows.items() if rel.startswith("koios/")}
    return [
        *_one_file(rows, MCML, "mcml.v", MCML_MS, None, expected),
        *_one_file(rows, LENET, "lenet.v", LENET_MS, LENET_RSS_BYTES, expected),
        _set_target(f"the {VTR_FILES} VTR files, total proc --noopt", vtr, VTR_FILES, VTR_MS,
                    sum("/" not in rel for rel in expected)),
        _set_target(f"the {KOIOS_FILES} Koios files, total proc --noopt", koios, KOIOS_FILES,
                    KOIOS_MS, sum(rel.startswith("koios/") for rel in expected)),
    ]


PROFILES: dict[str, Callable[..., list[Target]]] = {"proc": proc_targets}


def _fields(row: Row, keys: Sequence[str]) -> str:
    return " ".join(f"{key}={row[key]}" for key in keys if key in row)


def _worst_budget(rows: Mapping[str, Row]) -> str | None:
    """The row whose byte-budget checks cost the largest share of proc --noopt (finding 6)."""
    best, share = None, 0.0
    for rel, row in rows.items():
        noopt = float(row.get("noopt_ms", "0"))
        part = float(row.get("budget_ms", "0")) / noopt if noopt > 0 else 0.0
        if part > share:
            best, share = rel, part
    return None if best is None else f"{best} {share * 100:.1f}%"


def report(profile: str, rows: Mapping[str, Row],
           expected: frozenset[str] = frozenset()) -> tuple[list[str], bool]:
    """(lines to print, whether a target was missed). The estimate terms and step times of the
    files a target names follow a miss (always for lenet.v): investigated, never relaxed."""
    if profile not in PROFILES:
        raise ValueError(f"unknown bench profile {profile!r} (known: {', '.join(PROFILES)})")
    short = {rel: missing_columns(row) for rel, row in rows.items() if missing_columns(row)}
    whole = {rel: row for rel, row in rows.items() if rel not in short}
    targets = PROFILES[profile](whole, expected)
    targets += [Target(f"{rel} row", "MISSED", f"missing columns: {', '.join(gone)}")
                for rel, gone in short.items()]
    lines = ["bench targets (spec 19):"]
    lines += [f"  {t.verdict:<6} {t.name}: {t.detail}" for t in targets]
    missed = any(t.verdict == "MISSED" for t in targets)
    for rel in (MCML, LENET):
        if rel in rows and (missed or rel == LENET):
            lines.append(f"  steps of {rel}: {_fields(rows[rel], STEP_COLUMNS)}")
            lines.append(f"  estimate terms of {rel}: {_fields(rows[rel], ESTIMATE_TERMS)}")
    worst = _worst_budget(rows)
    if worst is not None:
        lines.append(f"  largest byte-budget share of noopt_ms: {worst}")
    return lines, missed


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


def expect_paths(files: Sequence[str]) -> frozenset[str]:
    """The relative paths an expect-fail list names (``#`` lines and blanks skipped)."""
    out: set[str] = set()
    for name in files:
        for raw in Path(name).read_text("utf-8").splitlines():
            if raw.strip() and not raw.lstrip().startswith("#"):
                out.add(raw.split()[0])
    return frozenset(out)


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
        print(f"{TOOL}: smoke: {len(files)} files, every one of the {len(PROC_COLUMNS)} columns")
    return EXIT_FAIL if failed else EXIT_OK


def _parse_size(text: str) -> int:
    match = re.fullmatch(r"(\d+)([KMGT]?)", text.strip().upper())
    if not match:
        raise ValueError(f"bad size {text!r} (e.g. 3G)")
    shift = 10 * ("", "K", "M", "G", "T").index(match.group(2))
    return int(match.group(1)) << shift


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
            lines, missed = report("proc", parse_sweep_log(text),
                                   expect_paths(args.expect_fail))
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
