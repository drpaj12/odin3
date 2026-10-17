"""opt-nightly: the nightly VTR legs of opt-equiv and opt-diff, run by hand (2F spec 20.2, 20.3,
plan Task 20, ruling I9).

Usage::

    opt-nightly --odin3 O --yosys Y [--set vtr31|all64] [--leg both|equiv|diff] [--xl] [--keep DIR]
                [--expect-fail F]... [--diff-expect F] [--root R] [--tops F] [--techlib L]
                [--mem-cap C] [--cap-for SUFFIX=C]... [--limit N] [--only NAME]... [--timeout S]

On each file of the set, smallest first, it runs ``opt-equiv`` (the refinement of every sub-pass
call of the Phase 2 script) and ``opt-diff`` once for each of the five scripts of spec 20.3 (the
Phase 2 script, ``opt -full``, ``opt -fast``, ``opt -fast -noff``, ``opt -nodffe -nosdff``), each as
a subprocess through ``memcap.run``, one at a time. It prints one line per (file, tool or script)
and a summary line.

- ``--leg equiv`` or ``diff`` runs one tool only (the CTests ``opt_equiv_vtr`` and
  ``opt_diff_vtr``); the default runs both.
- ``--set vtr31`` (the default) is the 31 ``verilog/*.v`` files, ``mcml.v`` last. ``--set all64`` is
  those and the 33 ``verilog/koios/*.v`` files as one set by size, ``koios/lenet.v`` last. The set
  is read from ``--root`` (default ``$VTR_ROOT/vtr_flow/benchmarks/verilog``).
- opt-equiv exit 4 (``unproven``: the checker hit a size limit on a proof and the whole-script
  cec decided nothing) is a failure an expect-fail line lists as ``untested for now (size
  limit)``, never hard (the timeout and kill words in its lines are the checker's); an unlisted one
  is printed ``UNPROVEN`` and fails the run like any unlisted failure. Odin III's own cap kill is
  exit 3, hard. opt-diff exit 4 (Yosys, the oracle, killed
  by the memory cap: no verdict on Odin III) is listed the same way.
- ``--expect-fail F`` (repeatable; default ``tests/opt/vtr_expect_fail.txt`` and
  ``tests/opt/vtr_nightly_xl.txt``) lists ``<path> [needle] <reason>`` lines. A listed file must
  fail (a tool exits non-zero; the needle must appear in the output); an unlisted failure, a
  listed file that passes fully, and a hard failure (a cap kill or a timeout) fail the run. A
  reason that starts with ``xl`` marks a file for the oversized run: without ``--xl`` it is
  skipped, with ``--xl`` only such files run, each must pass, and an empty selection exits 2.
- ``--diff-expect F`` (default ``tests/opt/diff_expect.txt``) goes to ``opt-diff --expect``.
- ``--mem-cap C`` per child (default 3G); ``--cap-for koios/lenet.v=5G`` raises one file's cap.
- ``--limit N`` runs only the first N files of the selection (a smoke run). ``--only NAME``
  (repeatable) runs only the files of that name or relative path (``--only ch_intrinsics.v``):
  the re-run of a few designs after a fix, in the set's size order; a NAME that matches no file of
  the set exits 2. ``--keep DIR`` keeps every tool's work directory under DIR.

Exit codes: 0 pass; 1 an unexpected result; 2 usage or input error; 77 skipped (cannot cap).
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path

import elab_sweep
import memcap
import opt_equiv
import toolutil
from elab_equiv import format_cap

TOOL = "opt-nightly"
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_SKIP = 0, 1, 2, 77
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
OPT_EQUIV = HERE / "opt_equiv.py"
OPT_DIFF = HERE / "opt_diff.py"
DEFAULT_EXPECT = (REPO / "tests/opt/vtr_expect_fail.txt", REPO / "tests/opt/vtr_nightly_xl.txt")
DEFAULT_DIFF_EXPECT = REPO / "tests/opt/diff_expect.txt"
SCRIPTS = ("phase2", "opt -full", "opt -fast", "opt -fast -noff", "opt -nodffe -nosdff")
SETS = ("vtr31", "all64")
LAST = {"vtr31": "mcml.v", "all64": "koios/lenet.v"}
DEFAULT_CAP = 3 << 30
TIMEOUT_S = 3600.0
CAP_KILLED = (-9, 137)  # SIGKILL: the cap of a child, seen as -9 or as 128 + 9
HARD_MARKS = ("out of memory (cap ", "timed out", "killed by signal")
_SLUG = re.compile(r"[^A-Za-z0-9]+")


class UsageError(Exception):
    """A bad argument or input (exit code 2)."""


class SkipRun(Exception):
    """A tool exited 77: the machine cannot cap its children (exit code 77)."""


@dataclass(frozen=True)
class Unit:
    """One tool run on one file: the line label and the tool's own arguments."""

    label: str
    tool: Path
    args: tuple[str, ...]


def units(leg: str = "both") -> list[Unit]:
    """opt-equiv, then opt-diff with each of the five scripts of spec 20.3 (``leg`` selects)."""
    found = [Unit("opt-equiv", OPT_EQUIV, ())] if leg in ("both", "equiv") else []
    if leg in ("both", "diff"):
        for script in SCRIPTS:
            found.append(Unit(f"opt-diff[{script}]", OPT_DIFF, ("--script", script)))
    return found


def collect_set(root: Path, which: str) -> list[elab_sweep.Item]:
    """The files of ``which`` under ``root``, smallest first, the set's last file at the end."""
    if which not in SETS:
        raise UsageError(f"--set {which}: one of {', '.join(SETS)}")
    files = sorted(root.glob("*.v"))
    if which == "all64":
        files += sorted((root / "koios").glob("*.v"))
    if not files:
        raise UsageError(f"{root}: no benchmark .v files")
    try:
        items = elab_sweep.collect([str(f) for f in files], root)
    except elab_sweep.UsageError as exc:
        raise UsageError(str(exc)) from exc
    last = [i for i in items if i.rel == LAST[which]]
    return [i for i in items if i.rel != LAST[which]] + last


def select(items: Sequence[elab_sweep.Item], expect: dict[str, elab_sweep.Expect], xl: bool,
           limit: int | None = None, only: Sequence[str] = ()
           ) -> tuple[list[elab_sweep.Item], list[elab_sweep.Item]]:
    """(to run, skipped): ``elab_sweep.split_xl``; an empty ``--xl`` selection is a usage error,
    as is an ``--only`` name that matches no file."""
    run, skipped = elab_sweep.split_xl(items, expect, xl)
    if xl and not run:
        raise UsageError("--xl selects no file: no collected file is listed `xl` in the "
                         "expect-fail files")
    for name in only:
        if not any(i.rel == name or i.path.name == name for i in items):
            raise UsageError(f"--only {name}: no such file in the set")
    if only:
        run = [i for i in run if i.rel in only or i.path.name in only]
    if limit is not None:
        run = run[:limit]
    return run, skipped


def parse_caps(values: Sequence[str]) -> dict[str, int]:
    caps: dict[str, int] = {}
    for value in values:
        suffix, sep, size = value.partition("=")
        if not sep or not suffix:
            raise UsageError(f"--cap-for {value}: expected SUFFIX=SIZE")
        try:
            caps[suffix] = elab_sweep.parse_size(size)
        except elab_sweep.UsageError as exc:
            raise UsageError(str(exc)) from exc
    return caps


def cap_of(item: elab_sweep.Item, default: int, caps: dict[str, int]) -> int:
    for suffix, cap in caps.items():
        if item.rel == suffix or item.rel.endswith("/" + suffix):
            return cap
    return default


@dataclass(frozen=True)
class Config:
    odin3: str
    yosys: str
    root: Path
    cap: int
    caps: dict[str, int]
    tops: dict[str, str]
    techlib: str | None
    diff_expect: Path
    keep: Path | None
    timeout: float
    leg: str = "both"


def unit_argv(cfg: Config, unit: Unit, item: elab_sweep.Item, cap: int) -> list[str]:
    top = cfg.tops.get(item.rel) or cfg.tops.get(item.path.name)
    argv = [sys.executable, str(unit.tool), "--odin3", cfg.odin3, "--yosys", cfg.yosys,
            "--mem-cap", str(cap), *(["--top", top] if top else []),
            *(["--techlib", cfg.techlib] if cfg.techlib else []), *unit.args]
    if unit.tool == OPT_DIFF:
        argv += ["--expect", str(cfg.diff_expect), "--root", str(cfg.root)]
    if cfg.keep is not None:
        work = cfg.keep / _SLUG.sub("_", item.rel) / _SLUG.sub("_", unit.label)
        work.mkdir(parents=True, exist_ok=True)
        argv += ["--work", str(work)]
    return [*argv, str(item.path)]


@dataclass(frozen=True)
class Result:
    code: int  # the tool's exit code
    secs: float
    text: str
    hard: bool  # a cap kill or a timeout: never an expected failure
    unproven: bool = False  # opt-equiv exit 4: a size limit of the checker, listable


def run_unit(cfg: Config, unit: Unit, item: elab_sweep.Item, cap: int) -> Result:
    start = time.monotonic()
    here = str(HERE.parent / "elab-check")  # the tools import memcap from there
    env = {**os.environ, "PYTHONPATH": os.pathsep.join([here, os.environ.get("PYTHONPATH", "")])}
    try:
        done = memcap.run(unit_argv(cfg, unit, item, cap), cap, capture_output=True, env=env,
                          timeout=cfg.timeout)
    except subprocess.TimeoutExpired:
        return Result(1, time.monotonic() - start, f"timed out after {cfg.timeout:.0f} s", True)
    except OSError as exc:
        return Result(1, time.monotonic() - start, f"cannot run {unit.tool.name}: {exc}", True)
    text = (done.stdout or "") + (done.stderr or "")
    # opt-equiv exit 4: only the checker hit a size limit (a proof timeout or the checker's cap
    # kill: its lines say "timed out" or "killed"); Odin III's own cap kill or crash is exit 3.
    killed = unit.tool == OPT_EQUIV and done.returncode in CAP_KILLED
    if killed:  # opt-equiv itself (its Python, not Odin III: that child's kill is its exit 3)
        text += (f"opt-equiv: unproven: opt-equiv itself was killed by its memory cap "
                 f"({format_cap(cap)}); the snapshot is too big for the checker\n")
    unproven = (unit.tool == OPT_EQUIV and (done.returncode == opt_equiv.EXIT_UNPROVEN or killed)
                or unit.tool == OPT_DIFF and done.returncode == opt_equiv.EXIT_UNPROVEN)
    hard = not unproven and (
        toolutil.is_crash(done.returncode) or done.returncode == opt_equiv.EXIT_CRASH
        or any(mark in text for mark in HARD_MARKS)
        or any(mark in text for mark in elab_sweep.OPT_TOOL_ABORT))
    return Result(done.returncode, time.monotonic() - start, text, hard, unproven)


Runner = Callable[[Config, Unit, elab_sweep.Item, int], Result]


@dataclass
class Tally:
    files: int = 0
    skipped: int = 0
    ok: int = 0
    listed: int = 0
    bad: int = 0  # unexpected failures, hard failures, stale entries
    units: int = 0
    changed: int = 0  # opt-equiv: the calls that changed something, over the ok runs


def finish_line(text: str) -> str:
    return elab_sweep.first_error(text) or "no output"


def run_file(cfg: Config, item: elab_sweep.Item, entry: elab_sweep.Expect | None,
             tally: Tally, runner: Runner, out: Callable[[str], None]) -> None:
    """Every unit on one file, one line each; a listed file must fail in some unit."""
    cap = cap_of(item, cfg.cap, cfg.caps)
    failed_any = False
    for unit in units(cfg.leg):
        res = runner(cfg, unit, item, cap)
        tally.units += 1
        tag = f"{TOOL}: {item.rel} [{unit.label}]"
        if res.code == EXIT_SKIP:
            raise SkipRun
        if res.code == 0:
            if unit.label == "opt-equiv":
                tally.changed += sum(int(n) for n in
                                     elab_sweep.OPT_CHANGED_CALLS.findall(res.text))
            out(f"{tag}: ok ({res.secs:.1f} s, cap {format_cap(cap)})")
            tally.ok += 1
            continue
        failed_any = True
        why = finish_line(res.text)
        if entry is not None and not res.hard and entry.needle in res.text:
            out(f"{tag}: fails as listed ({entry.reason}): {why}")
            tally.listed += 1
        else:
            kind = "HARD FAIL" if res.hard else "UNPROVEN" if res.unproven else "FAIL"
            extra = ""
            if entry is not None and not res.hard:
                extra = f" (listed, but the needle '{entry.needle}' is missing)"
            out(f"{tag}: {kind}{extra}: {why}")
            tally.bad += 1
    if entry is not None and not failed_any:
        out(f"{TOOL}: {item.rel}: STALE entry in the expect-fail list: passes every check "
            f"({entry.reason})")
        tally.bad += 1


def summary(tally: Tally, which: str, xl: bool) -> str:
    return (f"{TOOL}: summary: set {which}{' --xl' if xl else ''}: {tally.files} files "
            f"({tally.skipped} skipped as xl or unselected), {tally.units} runs: {tally.ok} ok, "
            f"{tally.listed} fail as listed, {tally.bad} UNEXPECTED")


def execute(args: argparse.Namespace, runner: Runner = run_unit,
            out: Callable[[str], None] = lambda s: print(s, flush=True)) -> int:
    root = args.root or Path(os.environ.get("VTR_ROOT", "")) / "vtr_flow/benchmarks/verilog"
    if not root.is_dir():
        raise UsageError(f"{root}: no such directory (set VTR_ROOT or pass --root)")
    root = root.resolve()
    expect = elab_sweep.merge_expect(args.expect_fail or list(DEFAULT_EXPECT))
    items = collect_set(root, args.set)
    run, _ = select(items, expect, args.xl, args.limit, getattr(args, "only", None) or ())
    if not run:
        raise UsageError("no file selected (an empty set, --limit 0, or --xl with no xl file)")
    cfg = Config(args.odin3, args.yosys, root, args.mem_cap, parse_caps(args.cap_for),
                 elab_sweep.read_tops(args.tops) if args.tops else {}, args.techlib,
                 args.diff_expect, args.keep, args.timeout, args.leg)
    tally = Tally(files=len(run), skipped=len(items) - len(run))
    for item in run:
        entry = expect.get(item.rel)  # an `xl` entry only selects the file: it must pass
        run_file(cfg, item, None if entry is not None and entry.xl else entry, tally, runner, out)
    out(summary(tally, args.set, args.xl))
    short = cfg.leg != "diff" and tally.changed < args.min_changed_total
    if short:  # a do-nothing odin3 passes every file (2F-6 re-review N3)
        out(f"{TOOL}: FAIL: opt-equiv: {tally.changed} changing calls in total, want at least "
            f"{args.min_changed_total}")
    return EXIT_FAIL if tally.bad or short else EXIT_OK


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--set", choices=SETS, default="vtr31", help="vtr31 (default) or all64")
    ap.add_argument("--leg", choices=("both", "equiv", "diff"), default="both",
                    help="opt-equiv only, opt-diff (five scripts) only, or both (default)")
    ap.add_argument("--xl", action="store_true", help="only the files listed `xl`")
    ap.add_argument("--keep", type=Path, help="keep every tool's work directory under DIR")
    ap.add_argument("--expect-fail", type=Path, action="append", metavar="F",
                    help="repeatable; default tests/opt/vtr_expect_fail.txt + vtr_nightly_xl.txt")
    ap.add_argument("--diff-expect", type=Path, default=DEFAULT_DIFF_EXPECT, metavar="F")
    ap.add_argument("--root", type=Path, help="verilog/ of the VTR benchmarks (default $VTR_ROOT)")
    ap.add_argument("--tops", type=Path, help="one `<file> <top>` per line")
    ap.add_argument("--techlib", help="read this tech library (.o3lib) first")
    ap.add_argument("--mem-cap", type=elab_sweep.parse_size, default=DEFAULT_CAP, metavar="SIZE",
                    help="per child (default 3G)")
    ap.add_argument("--cap-for", action="append", default=[], metavar="SUFFIX=SIZE")
    ap.add_argument("--min-changed-total", type=int, default=0, metavar="N",
                    help="fail (exit 1) unless opt-equiv saw at least N changing calls in total")
    ap.add_argument("--limit", type=int, metavar="N", help="only the first N files (a smoke run)")
    ap.add_argument("--only", action="append", default=[], metavar="NAME",
                    help="only this file (name or relative path); repeatable")
    ap.add_argument("--timeout", type=float, default=TIMEOUT_S, metavar="S",
                    help="per tool run (default 3600)")
    return ap.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    try:
        return execute(args)
    except SkipRun:
        print(f"{TOOL}: skipped: a child cannot be capped", file=sys.stderr)
        return EXIT_SKIP
    except (UsageError, elab_sweep.UsageError, ValueError, OSError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP
    return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())
