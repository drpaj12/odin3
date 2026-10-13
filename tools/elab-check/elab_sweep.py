"""elab-sweep: a corpus of Verilog files through odin3's elaboration (2D spec §14.5, plan Task 26).

Usage::

    elab-sweep --odin3 O [--equiv --yosys Y --equiv-check E | --bench B [--bench-report P]]
               [--expect-fail F]...
               [--jobs J] [--mem-cap C] [--cap-for SUFFIX=C]... [--tops F] [--golden-root D]
               [--root R] [--xl] [--skip-over BYTES] [--partial] <dir or file>...

Collects every ``.v`` under the directories (and the files) given and sorts them **all together**
by size, smallest first (ties by relative path): a header-only ``.vh`` is never run alone, and a
multi-file micro runs through its includer (the includer's ``include`` reads the rest). Each file
runs one child at a time (``--jobs J`` at most J), every child under ``memcap.run`` (plan point
(n)): ``--mem-cap`` per child (default 4G; ``--cap-for koios/lenet.v=5G`` raises one file's cap and
runs it alone). A child killed by its cap is reported ``out of memory (cap 4G)``. The modes:

- default: ``O [--top T] --elab-only FILE`` (``read_verilog FILE; stats``). ``T`` is the first
  ``.model`` of the micro's Parmys golden (``micro-top``, point (o); ``--golden-root``) or the
  file's line of ``--tops F`` (one ``<file> <top>`` per line, relative path or file name), else
  none (the top is found).
- ``--equiv`` (needs ``--yosys`` and ``--equiv-check``): ``elab-equiv`` on the file (§14.3).
  Its exit 3 is an ``oracle crash`` (Yosys died of a signal, e.g. 0.55 on recursive
  instantiation): counted and shown, neither a mismatch nor a 2D failure.
- ``--bench B``: ``B --one FILE [--techlib T]``; the child's stdout line follows the file's line as
  ``bench:``. ``--bench-report P`` then prints the targets of profile ``P`` from those lines
  (``bench_report.py``: ``elab`` is spec §15's table) and fails the run when one is MISSED.

``--expect-fail F`` (repeatable) lists ``<relative path> <one-line reason>`` lines (blank lines
and ``#`` comments ignored), the path relative to ``--root`` when under it, else to the directory
argument it was found under, else the file's name. An unlisted failure, a listed file that
passes and (without ``--partial``) a listed path that names no collected file fail the run. A
reason that starts with ``xl`` (``koios/lenet.v xl: Yosys side ~7 GB``) marks a file for the
oversized run: without ``--xl`` it is skipped, with ``--xl`` only such files run and each must
pass. ``--skip-over BYTES`` skips (as ``xl``) any file larger than BYTES.

An ASan ``odin3`` (or ``B``) with no ``systemd-run --user`` cannot be capped: exit 77, CTest's
skip. ``--jobs J`` is refused when ``J`` times the cap exceeds 60% of MemTotal or the heavy
slice's MemoryMax.

Library use (``proc-diff``'s sweeps, 2E Tasks 11-13): ``collect``, ``read_expect``, ``read_tops``,
``top_for`` and ``split_xl`` below.

Exit codes: 0 pass; 1 an unexpected result; 2 usage or input error; 77 skipped.
"""

from __future__ import annotations

import argparse
import importlib.machinery
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Sequence
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType

import bench_report
import memcap
from elab_equiv import GIB, MIB, format_cap
from run_rows_equiv import parse_expect_line

TOOL = "elab-sweep"
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_SKIP = 0, 1, 2, 77
TIMEOUT_S = 3600  # a file past an hour is that file's failure
TAIL_LINES = 3
SUFFIX = ".v"
EQUIV_OK, EQUIV_DIFFER, EQUIV_ERROR, EQUIV_CRASH = 0, 1, 2, 3  # elab-equiv's exit codes
ELAB_EQUIV = Path(__file__).resolve().parent / "elab_equiv.py"
_XL_REASON = re.compile(r"xl\b:?\s*")
_SIZE = re.compile(r"(\d+)([KMG]?)", re.IGNORECASE)
_UNITS = {"": 1, "K": 1 << 10, "M": MIB, "G": GIB}


class UsageError(Exception):
    """A bad argument or input file (exit code 2)."""


@dataclass(frozen=True)
class Item:
    path: Path
    rel: str
    size: int


@dataclass(frozen=True)
class Expect:
    reason: str
    xl: bool
    needle: str = ""  # must appear in the child's output (``<path> [needle] <reason>``)


@dataclass(frozen=True)
class Outcome:
    kind: str  # "ok", "fail", "crash" (an oracle crash) or "skip"
    secs: float
    why: str = ""
    output: str = ""
    hard: bool = False  # a signal, a memory-cap kill, a timeout or a child that cannot run


def parse_size(text: str) -> int:
    """``4G``, ``512M``, ``100K`` or a plain byte count."""
    match = _SIZE.fullmatch(text.strip())
    if match is None:
        raise UsageError(f"{text!r} is not a size (4G, 512M, 100K or bytes)")
    return int(match.group(1)) * _UNITS[match.group(2).upper()]


# ---- collection (the library half) ------------------------------------------------------------


def relative(path: Path, base: Path, root: Path | None) -> str:
    if root is not None and path.is_relative_to(root):
        return path.relative_to(root).as_posix()
    if path.is_relative_to(base):
        return path.relative_to(base).as_posix()
    return path.name


def collect(paths: Sequence[str], root: Path | None = None, suffix: str = SUFFIX) -> list[Item]:
    """Every file with ``suffix`` under the directory arguments, and the file arguments, all
    together, smallest first (ties by relative path). ``.vh`` headers never match ``.v``."""
    found: dict[Path, Item] = {}
    for arg in paths:
        given = Path(arg).resolve()
        if given.is_dir():
            files = sorted(p for p in given.rglob(f"*{suffix}") if p.is_file())
            base = given
        elif given.is_file():
            files, base = [given], given.parent
        else:
            raise UsageError(f"{arg}: no such file or directory")
        for path in files:
            found.setdefault(path, Item(path, relative(path, base, root), path.stat().st_size))
    return sorted(found.values(), key=lambda item: (item.size, item.rel))


def read_expect(path: Path) -> dict[str, Expect]:
    """The expect-fail list: relative path -> Expect. Every entry needs a reason; the
    ``[needle]`` between path and reason (the syntax of run-rows-equiv's list) is optional."""
    expect: dict[str, Expect] = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        try:
            parsed = parse_expect_line(raw)
        except ValueError as exc:
            raise UsageError(f"{path}:{number}: {exc}") from exc
        if parsed is None:
            continue
        key, needle, reason = parsed
        if key in expect:
            raise UsageError(f"{path}:{number}: '{key}' is listed twice")
        expect[key] = Expect(reason, _XL_REASON.match(reason) is not None, needle)
    return expect


def merge_expect(paths: Sequence[Path]) -> dict[str, Expect]:
    """Several expect-fail files as one list (a path may be in one file only)."""
    merged: dict[str, Expect] = {}
    for path in paths:
        for rel, entry in read_expect(path).items():
            if rel in merged:
                raise UsageError(f"{path}: '{rel}' is listed in another expect-fail file too")
            merged[rel] = entry
    return merged


def read_tops(path: Path) -> dict[str, str]:
    """``--tops F``: one ``<file> <top>`` per line (blank lines and ``#`` comments skipped)."""
    tops: dict[str, str] = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 2:
            raise UsageError(f"{path}:{number}: expected '<file> <top>'")
        tops[parts[0]] = parts[1]
    return tops


def load_micro_top() -> ModuleType:
    """The ``micro-top`` script (no ``.py`` suffix) as a module."""
    path = str(Path(__file__).resolve().parent / "micro-top")
    loader = importlib.machinery.SourceFileLoader("micro_top", path)
    spec = importlib.util.spec_from_loader("micro_top", loader)
    if spec is None:
        raise ImportError(path)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def top_for(item: Item, tops: dict[str, str], golden_root: Path | None) -> str | None:
    """The file's ``--top``: its ``--tops`` line (by relative path, else by name), else the first
    ``.model`` of its Parmys golden when one exists, else None."""
    for key in (item.rel, item.path.name):
        if key in tops:
            return tops[key]
    if golden_root is None:
        return None
    micro = load_micro_top()
    try:
        return str(micro.first_model(micro.golden_path(golden_root, str(item.path))))
    except (OSError, ValueError):
        return None


def split_xl(items: Sequence[Item], expect: dict[str, Expect], xl: bool,
             skip_over: int | None = None) -> tuple[list[Item], list[Item]]:
    """(to run, skipped). Without ``xl`` the files listed ``xl`` and those over ``skip_over``
    bytes are skipped; with it only the files listed ``xl`` run."""
    run: list[Item] = []
    skipped: list[Item] = []
    for item in items:
        entry = expect.get(item.rel)
        listed_xl = entry is not None and entry.xl
        if xl:
            (run if listed_xl else skipped).append(item)
        elif listed_xl or (skip_over is not None and item.size > skip_over):
            skipped.append(item)
        else:
            run.append(item)
    return run, skipped


# ---- limits -----------------------------------------------------------------------------------


def slice_limit() -> int | None:
    """MemoryMax of the heavy slice this run is inside, else None."""
    slice_name = os.environ.get(memcap.SLICE_ENV)
    if not slice_name or shutil.which("systemctl") is None:
        return None
    try:
        done = subprocess.run(["systemctl", "--user", "show", "-p", "MemoryMax", "--value",
                               slice_name], capture_output=True, text=True, check=False,
                              timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    text = done.stdout.strip()
    return int(text) if text.isdigit() else None


def check_jobs(jobs: int, cap: int, alone: Sequence[int] = ()) -> None:
    """Refuse J x cap above 60% of MemTotal, or above the heavy slice's MemoryMax. A ``--cap-for``
    file larger than ``cap`` runs alone (``Sweep.run``), so each ``alone`` cap is checked as one
    job against the same limits."""
    if jobs < 1:
        raise UsageError(f"--jobs {jobs} is not positive")
    total = memcap.mem_total()
    limit = total * memcap.CAP_FRACTION_PERCENT // 100
    in_slice = slice_limit()
    for width, size in [(jobs, cap), *((1, big) for big in alone if big > cap)]:
        label = f"--jobs {width} x --mem-cap {format_cap(size)}" if width > 1 \
            else f"--cap-for {format_cap(size)} (alone)"
        if width * size > limit:
            raise UsageError(f"{label} = {format_cap(width * size)} exceeds "
                             f"{memcap.CAP_FRACTION_PERCENT}% of MemTotal ({limit / GIB:.1f}G)")
        if in_slice is not None and width * size > in_slice:
            raise UsageError(f"{label} exceeds the heavy slice's MemoryMax "
                             f"({format_cap(in_slice)})")


# ---- running one file -------------------------------------------------------------------------


def first_error(text: str) -> str:
    """The first error line of a run's output, else its last few lines."""
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    for line in lines:
        if "error" in line.lower():
            return line.removeprefix("odin3: error: ")
    return " | ".join(lines[-TAIL_LINES:])


def child_why(done: subprocess.CompletedProcess[str], cap: int) -> str:
    """Why a child that exited non-zero failed: its cap, a signal, or its first error."""
    code = done.returncode
    text = (done.stdout or "") + (done.stderr or "")
    if code in (-9, 137):
        return f"out of memory (cap {format_cap(cap)})"
    if code < 0:
        return f"killed by signal {-code}: {first_error(text)}"
    return f"exit {code}: {first_error(text)}"


def child_outcome(done: subprocess.CompletedProcess[str], cap: int, secs: float) -> Outcome:
    """The failed Outcome of a child that exited non-zero: its whole output is kept for the
    expect-fail needle, and a signal or cap kill is ``hard`` (never an expected failure)."""
    text = (done.stdout or "") + (done.stderr or "")
    return Outcome("fail", secs, child_why(done, cap), text, hard=done.returncode < 0
                   or done.returncode == 137)


@dataclass(frozen=True)
class Config:
    odin3: str
    yosys: str | None
    equiv_check: str | None
    bench: str | None
    cap: int
    caps: dict[str, int]
    tops: dict[str, str]
    golden_root: Path | None
    techlib: str | None = None
    bench_report: str | None = None

    def cap_of(self, item: Item) -> int:
        for suffix, cap in self.caps.items():
            if item.rel == suffix or item.rel.endswith("/" + suffix):
                return cap
        return self.cap

    def mode(self) -> str:
        if self.bench:
            return "bench"
        return "equiv" if self.yosys else "elab"


def run_child(argv: Sequence[str], cap: int, start: float) -> tuple[subprocess.CompletedProcess[str]
                                                                    | None, Outcome | None]:
    """(result, None), or (None, a failed Outcome) when the child cannot run or times out."""
    try:
        return memcap.run(list(argv), cap, capture_output=True, timeout=TIMEOUT_S), None
    except subprocess.TimeoutExpired:
        return None, Outcome("fail", time.monotonic() - start, f"timed out after {TIMEOUT_S} s",
                         hard=True)
    except OSError as exc:
        return None, Outcome("fail", time.monotonic() - start, f"cannot run {argv[0]}: {exc}",
                             hard=True)


def run_elab(cfg: Config, item: Item, cap: int) -> Outcome:
    start = time.monotonic()
    top = top_for(item, cfg.tops, cfg.golden_root)
    argv = [cfg.odin3, *(["--top", top] if top else []), "--elab-only", str(item.path)]
    if cfg.techlib:  # the arch's hard blocks (adder, multiply, *_port_ram) come from a tech library
        argv = [cfg.odin3, *(["--top", top] if top else []), "-p",
                f"read_techlib {cfg.techlib}; read_verilog --nolatches {item.path}; stats"]
    done, failed = run_child(argv, cap, start)
    if done is None:
        assert failed is not None
        return failed
    secs = time.monotonic() - start
    if done.returncode == 0:
        return Outcome("ok", secs, output=done.stdout or "")
    return child_outcome(done, cap, secs)


def run_bench(cfg: Config, item: Item, cap: int) -> Outcome:
    start = time.monotonic()
    assert cfg.bench is not None
    argv = [cfg.bench, "--one", str(item.path)]
    if cfg.techlib:
        argv += ["--techlib", cfg.techlib]
    done, failed = run_child(argv, cap, start)
    if done is None:
        assert failed is not None
        return failed
    secs = time.monotonic() - start
    if done.returncode == 0:
        return Outcome("ok", secs, output=(done.stdout or "").strip())
    return child_outcome(done, cap, secs)


def lock_fds() -> tuple[int, ...]:
    """The heavy lock's descriptor when open (tools/heavy-run holds it), for elab-equiv to pass
    on to its own memcap children."""
    try:
        os.fstat(memcap.LOCK_FD)
    except OSError:
        return ()
    return (memcap.LOCK_FD,)


def run_equiv(cfg: Config, item: Item, cap: int) -> Outcome:
    start = time.monotonic()
    assert cfg.yosys is not None and cfg.equiv_check is not None
    top = top_for(item, cfg.tops, cfg.golden_root)
    argv = [sys.executable, str(ELAB_EQUIV), "--odin3", cfg.odin3, "--yosys", cfg.yosys,
            "--equiv-check", cfg.equiv_check, "--mem-cap", str(cap), "--unmap-enables",
            *(["--top", top] if top else []), *(["--techlib", cfg.techlib] if cfg.techlib else []),
            str(item.path)]
    try:  # elab-equiv caps each of its own children
        done = subprocess.run(argv, capture_output=True, text=True, errors="replace",
                              timeout=TIMEOUT_S, check=False, pass_fds=lock_fds())
    except subprocess.TimeoutExpired:
        return Outcome("fail", time.monotonic() - start, f"timed out after {TIMEOUT_S} s",
                       hard=True)
    secs = time.monotonic() - start
    text = (done.stdout or "") + (done.stderr or "")
    code = done.returncode
    if code == EQUIV_OK:
        return Outcome("ok", secs, output=done.stdout or "")
    if code == EQUIV_CRASH:
        return Outcome("crash", secs, first_error(text.replace("elab-equiv: ", "")))
    if code == EXIT_SKIP:
        return Outcome("skip", secs, "cannot be capped")
    if code == EQUIV_DIFFER:
        return Outcome("fail", secs, "NOT equivalent: " + first_error(text), text)
    return Outcome("fail", secs, first_error(text) or f"exit {code}", text, hard=code < 0)


def run_file(cfg: Config, item: Item) -> Outcome:
    cap = cfg.cap_of(item)
    mode = cfg.mode()
    if mode == "bench":
        return run_bench(cfg, item, cap)
    return run_equiv(cfg, item, cap) if mode == "equiv" else run_elab(cfg, item, cap)


# ---- the sweep --------------------------------------------------------------------------------


def verdict(item: Item, outcome: Outcome, expect: dict[str, Expect], xl: bool) -> tuple[str, str]:
    """(class, line): class is ok, xfail, FAIL, XPASS, crash or skip."""
    head = f"{item.rel} ({item.size} B, {outcome.secs:.3f} s)"
    entry = expect.get(item.rel)
    listed = entry is not None and not entry.xl  # an xl entry is a run class, not an expectation
    if outcome.kind == "skip":
        return "skip", f"skip  {head}: {outcome.why}"
    if outcome.kind == "crash":
        if listed:
            assert entry is not None
            return "xfail", f"xfail {head}: {entry.reason} (oracle crash)"
        return "crash", f"crash {head}: oracle crash: {outcome.why}"
    if outcome.kind == "ok":
        if listed:
            assert entry is not None
            return "XPASS", f"XPASS {head}: listed as expected to fail ({entry.reason}) but passes"
        return "ok", f"ok    {head}"
    if listed:
        assert entry is not None
        if outcome.hard:  # a crash, cap kill or timeout is never the listed rejection
            return "FAIL", f"FAIL  {head}: {outcome.why} (listed, but a signal, a memory cap or " \
                           "a timeout is never an expected failure)"
        if entry.needle and entry.needle not in outcome.output:
            return "FAIL", (f"FAIL  {head}: fails differently from the listing: expected "
                            f"[{entry.needle}], got {outcome.why}")
        return "xfail", f"xfail {head}: {entry.reason}"
    return "FAIL", f"FAIL  {head}: {outcome.why}"


def parse_bench(text: str) -> dict[str, str]:
    """The columns of a bench line; a line that is not key=value words has none."""
    try:
        return bench_report.parse_line(text)
    except ValueError:
        return {}


class Sweep:
    def __init__(self, cfg: Config, expect: dict[str, Expect], jobs: int, xl: bool) -> None:
        self.cfg = cfg
        self.expect = expect
        self.jobs = jobs
        self.xl = xl
        self.counts = {"ok": 0, "xfail": 0, "FAIL": 0, "XPASS": 0, "crash": 0, "skip": 0}
        self.skipped_run = False
        self.bench_rows: dict[str, dict[str, str]] = {}

    def report(self, item: Item, outcome: Outcome) -> None:
        cls, line = verdict(item, outcome, self.expect, self.xl)
        print(line, flush=True)
        if outcome.kind == "ok" and self.cfg.mode() == "bench" and outcome.output:
            print(f"bench: {outcome.output}", flush=True)
            self.bench_rows[item.rel] = parse_bench(outcome.output)
        self.counts[cls] += 1
        if outcome.kind == "skip":
            self.skipped_run = True

    def run(self, items: Sequence[Item]) -> None:
        pending: list[tuple[Item, Future[Outcome]]] = []
        with ThreadPoolExecutor(max_workers=self.jobs) as pool:
            for item in items:
                if self.cfg.cap_of(item) > self.cfg.cap:  # a raised cap runs alone
                    self.drain(pending, 0)
                    self.report(item, run_file(self.cfg, item))
                    continue
                pending.append((item, pool.submit(run_file, self.cfg, item)))
                self.drain(pending, self.jobs)
            self.drain(pending, 0)

    def drain(self, pending: list[tuple[Item, Future[Outcome]]], keep: int) -> None:
        """Reports finished files in order until at most ``keep`` are in flight."""
        while len(pending) > keep:
            item, future = pending.pop(0)
            self.report(item, future.result())


def stale_entries(expect: dict[str, Expect], items: Sequence[Item], skipped: Sequence[Item],
                  xl: bool) -> list[str]:
    """Listed paths that name no collected file (entries of the other run class are not stale)."""
    seen = {item.rel for item in [*items, *skipped]}
    return sorted(rel for rel, entry in expect.items() if rel not in seen and entry.xl == xl)


def sweep(cfg: Config, args: argparse.Namespace, items: list[Item],
          expect: dict[str, Expect]) -> int:
    run, skipped = split_xl(items, expect, args.xl, args.skip_over)
    state = Sweep(cfg, expect, args.jobs, args.xl)
    start = time.monotonic()
    for item in skipped:
        reason = "xl run only" if args.xl else "xl (oversized)"
        print(f"skip  {item.rel} ({item.size} B): {reason}")
    state.run(run)
    stale = [] if args.partial else stale_entries(expect, items, skipped, args.xl)
    for rel in stale:
        print(f"STALE {rel}: listed as expected to fail but not in the corpus")
    counts = state.counts
    print(f"{TOOL}: {len(run) + len(skipped)} files, {counts['ok']} passed, {counts['xfail']} "
          f"expected failures, {counts['FAIL']} unexpected failures, {counts['XPASS']} listed but "
          f"passing, {counts['crash']} oracle crashes, {len(skipped) + counts['skip']} skipped, "
          f"{len(stale)} stale entries, {time.monotonic() - start:.1f} s")
    if state.skipped_run and not counts["ok"] + counts["xfail"]:
        return EXIT_SKIP
    missed = False
    if cfg.bench_report:
        lines, missed = bench_report.report(cfg.bench_report, state.bench_rows)
        print("\n".join(lines))
    return EXIT_FAIL if counts["FAIL"] or counts["XPASS"] or stale or missed else EXIT_OK


# ---- arguments --------------------------------------------------------------------------------


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n\n")[0] if __doc__
                                 else None)
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--equiv", action="store_true", help="run elab-equiv (the bridge) per file")
    ap.add_argument("--yosys", help="the Yosys 0.55 binary (--equiv)")
    ap.add_argument("--equiv-check", help="tools/equiv-check/equiv-check (--equiv)")
    ap.add_argument("--bench", metavar="B", help="start `B --one FILE` per file")
    ap.add_argument("--bench-report", metavar="PROFILE",
                    help="after a --bench sweep, print the targets of this profile (elab)")
    ap.add_argument("--expect-fail", type=Path, action="append", default=[],
                    help="<relative path> <reason> lines (repeatable)")
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--mem-cap", default="4G", help="per child (default 4G)")
    ap.add_argument("--cap-for", action="append", default=[], metavar="SUFFIX=CAP",
                    help="a larger cap for one file, which then runs alone (e.g. koios/lenet.v=5G)")
    ap.add_argument("--techlib", type=Path,
                    help="read this tech library (.o3lib) before each file: the hard blocks "
                         "(adder, multiply, single/dual_port_ram) the micros instantiate")
    ap.add_argument("--tops", type=Path, help="one `<file> <top>` per line")
    ap.add_argument("--golden-root", type=Path, default=None,
                    help="the Parmys golden tree for --top (default: micro-top's)")
    ap.add_argument("--root", type=Path, help="make listed paths relative to this directory")
    ap.add_argument("--xl", action="store_true", help="run only the files listed `xl`")
    ap.add_argument("--skip-over", type=parse_size, metavar="BYTES",
                    help="skip files larger than this as xl")
    ap.add_argument("--partial", action="store_true", help="a subset: skip the stale-entry check")
    ap.add_argument("inputs", nargs="+", help="directories or files")
    return ap.parse_args(argv)


def build_config(args: argparse.Namespace) -> Config:
    if args.equiv and args.bench:
        raise UsageError("--equiv and --bench cannot be combined")
    if args.bench_report and not args.bench:
        raise UsageError("--bench-report needs --bench")
    if args.bench_report and args.bench_report not in bench_report.PROFILES:
        raise UsageError(f"unknown bench profile {args.bench_report!r} "
                         f"(known: {', '.join(bench_report.PROFILES)})")
    if args.equiv and not (args.yosys and args.equiv_check):
        raise UsageError("--equiv needs --yosys and --equiv-check")
    caps: dict[str, int] = {}
    for spec in args.cap_for:
        suffix, _, size = spec.partition("=")
        if not suffix or not size:
            raise UsageError(f"--cap-for {spec!r}: expected SUFFIX=CAP")
        caps[suffix] = parse_size(size)
    cap = parse_size(args.mem_cap)
    for tool in (args.odin3, args.bench) if not args.equiv else (args.odin3,):
        if tool is not None and shutil.which(tool) is None:
            raise UsageError(f"{tool} is not an executable file")
    golden = args.golden_root
    if golden is None:
        golden = load_micro_top().DEFAULT_ROOT
    tops = read_tops(args.tops) if args.tops else {}
    return Config(os.path.abspath(shutil.which(args.odin3) or args.odin3),
                  _abs(args.yosys) if args.equiv else None,
                  _abs(args.equiv_check) if args.equiv else None,
                  _abs(args.bench) if args.bench else None, cap, caps, tops, golden,
                  _abs(args.techlib) if args.techlib else None, args.bench_report)


def _abs(tool: str | None) -> str | None:
    if tool is None:
        return None
    return os.path.abspath(shutil.which(tool) or tool)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        cfg = build_config(args)
        check_jobs(args.jobs, cfg.cap, list(cfg.caps.values()))
        memcap.check_cap(max([cfg.cap, *cfg.caps.values()]))
        # an uncappable ASan binary skips the whole run before any file starts
        for tool in (cfg.odin3, cfg.bench):
            if tool is not None:
                memcap.wrap([tool], cfg.cap)
        expect = merge_expect(args.expect_fail)
        root = args.root.resolve() if args.root is not None else None
        items = collect(args.inputs, root)
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP
    except (UsageError, ValueError, OSError, RuntimeError) as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_USAGE
    if not items:
        print(f"{TOOL}: no input files", file=sys.stderr)
        return EXIT_USAGE
    return sweep(cfg, args, items, expect)


if __name__ == "__main__":
    sys.exit(main())
