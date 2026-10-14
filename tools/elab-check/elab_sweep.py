"""elab-sweep: a corpus of Verilog files through odin3's elaboration (2D spec §14.5, plan Task 26).

Usage::

    elab-sweep --odin3 O [--equiv --yosys Y --equiv-check E [--pass proc] | --proc-diff --yosys Y
               | --opt-equiv --yosys Y | --opt-diff --yosys Y [--opt-arg ARG]... | --opt-run
               | --bench B [--bench-report P]] [--trio auto|on|off] [--also-no-trio] [--nolatches]
               [--vtr-compat]
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
- ``--bench B``: ``B --one FILE [--techlib T]`` (``bench_elab`` for 2D, ``bench_proc`` for 2E;
  ``--techlib`` is passed on); the child's stdout line follows the file's line as ``bench:``.
  ``--bench-report P`` then prints the targets of profile ``P`` from those lines
  (``bench_report.py``: ``elab`` is spec §15's table, ``proc`` is 2E's spec §19) and fails the run
  when one is MISSED.
- ``--vtr-compat`` (with ``--equiv``): ``elab-equiv --vtr-compat`` (a non-constant ``initial x =
  e;`` is ``assign x = e;``, as in Yosys 0.55 and Parmys; the micros are the VTR flow's).
- ``--proc-diff`` (needs ``--yosys``; 2E Task 12): ``proc-diff`` on the file (Odin III's ``proc``
  against Yosys 0.55's on the same RTLIL; ``--top`` as in the default mode, ``--techlib`` passed
  on, ``--nolatches`` optional). With ``--equiv``, ``--pass proc`` makes the bridge end after
  ``proc`` (``elab-equiv --pass proc --async2sync``). ``--trio auto`` (the default) feeds the input
  after the pre-``proc`` passes once ``opt -nodffe -nosdff`` runs (2F-5) and passes ``--no-trio``
  until then, with a notice; ``--trio on`` requires it, ``--trio off`` never uses it;
  ``--also-no-trio`` runs each file a second time without it. A ``proc-diff`` exit 3 is a ``tie``
  (netlist-compare cannot order tied cells): a failure here, listable like any other.

- ``--opt-equiv`` / ``--opt-diff`` (need ``--yosys``; 2F Task 17): ``opt-equiv`` or ``opt-diff`` on
  the file (``--top`` as in the default mode; each ``--opt-arg=ARG`` goes to the child, e.g.
  ``--opt-arg=--script=phase2`` or ``--opt-arg=--expect=tests/opt/diff_expect.txt``). Exit 0 is a
  pass, 1 a difference, anything else the child's first error line.

- ``--opt-run`` (2F Task 18; Odin III only, no ``--yosys``): ``opt-run`` on the file, the Phase 2
  script with ``ODIN3_OK``, ``check`` FULL and the iteration cap (``--techlib`` passed on; ``--top``
  as in the default mode). The summary adds the largest ``opt`` iteration count and its file.
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
import dataclasses
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
OPT_CRASH = 3  # opt-equiv, opt-diff, opt-run: a child crashed, was cap-killed or timed out (hard)
TIMEOUT_S = 3600  # a file past an hour is that file's failure
TAIL_LINES = 3
SUFFIX = ".v"
EQUIV_OK, EQUIV_DIFFER, EQUIV_ERROR, EQUIV_CRASH = 0, 1, 2, 3  # elab-equiv's exit codes
ELAB_EQUIV = Path(__file__).resolve().parent / "elab_equiv.py"
PROC_DIFF = Path(__file__).resolve().parents[1] / "proc-check" / "proc_diff.py"
OPT_TOOLS = {"opt-equiv": Path(__file__).resolve().parents[1] / "opt-check" / "opt_equiv.py",
             "opt-diff": Path(__file__).resolve().parents[1] / "opt-check" / "opt_diff.py",
             "opt-run": Path(__file__).resolve().parents[1] / "opt-check" / "opt_run.py"}
TRIO_PROBE = "opt -nodffe -nosdff"  # the trio's own flags; fails until `opt` exists (2F-5)
# opt-equiv's summary line; the sweeps sum it (``--min-changed-total``)
OPT_CHANGED_CALLS = re.compile(r"^opt-equiv: (\d+) calls changed something", re.M)
# Exit 2 of an opt tool that is our own check failing, not the design: never an expected failure
OPT_TOOL_ABORT = ("yosys-refine exit", "netlist-compare cannot compare")
OPT_RUN_ITERATIONS = re.compile(r"iterations max=(\d+)")
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


# Texts of elab-equiv's own errors that are never a listed rejection: its children's cap kill and
# a tool it cannot run.
BRIDGE_HARD = ("out of memory (cap ", "is not an executable file", "cannot run ")


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
    libs: tuple[str, ...] = ()  # files read as library units first (--lib; 2D-7b)
    proc_diff: bool = False
    opt_tool: str = ""  # "opt-equiv", "opt-diff" (2F Task 17) or "opt-run" (Task 18)
    opt_args: tuple[str, ...] = ()
    pass_proc: bool = False  # --equiv: elab-equiv --pass proc
    no_trio: bool = False
    also_no_trio: bool = False
    nolatches: bool = False
    vtr_compat: bool = False  # --equiv: elab-equiv --vtr-compat

    def cap_of(self, item: Item) -> int:
        for suffix, cap in self.caps.items():
            if item.rel == suffix or item.rel.endswith("/" + suffix):
                return cap
        return self.cap

    def mode(self) -> str:
        if self.bench:
            return "bench"
        if self.proc_diff:
            return "procdiff"
        if self.opt_tool:
            return "opt"
        return "equiv" if self.equiv_check else "elab"


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
    if cfg.techlib or cfg.libs:  # hard blocks (adder, ...) from a tech library, soft ones --lib
        pre = f"read_techlib {cfg.techlib}; " if cfg.techlib else ""
        pre += "".join(f"read_verilog --lib --nomem2reg --nolatches {lib}; " for lib in cfg.libs)
        argv = [cfg.odin3, *(["--top", top] if top else []), "-p",
                f"{pre}read_verilog --nolatches {item.path}; stats"]
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


def proc_flags(cfg: Config) -> list[str]:
    """The bridge's flags for ``--pass proc``: the same as 2E's proc-rows (Task 11)."""
    if not cfg.pass_proc:
        return []
    return ["--pass", "proc", "--async2sync", *(["--no-trio"] if cfg.no_trio else [])]


def proc_diff_outcome(done: subprocess.CompletedProcess[str], secs: float) -> Outcome:
    """proc-diff's exit: 0 identical, 1 differs, 2 error, 3 a tie (netlist-compare cannot order
    the tied cells); the whole output is kept for the expect-fail needle."""
    text = (done.stdout or "") + (done.stderr or "")
    code = done.returncode
    if code == 0:
        return Outcome("ok", secs, output=done.stdout or "")
    if code == 1:
        return Outcome("fail", secs, "NOT identical: " + first_error(text), text)
    if code == 3:
        return Outcome("fail", secs, "tie: netlist-compare cannot order the tied cells", text)
    return Outcome("fail", secs, first_error(text) or f"exit {code}", text, hard=code < 0)


def run_proc_diff(cfg: Config, item: Item, cap: int) -> Outcome:
    """proc-diff on the file, once per input mode (``--also-no-trio`` adds ``--no-trio``)."""
    start = time.monotonic()
    assert cfg.yosys is not None
    top = top_for(item, cfg.tops, cfg.golden_root)
    modes = [cfg.no_trio] + ([True] if cfg.also_no_trio and not cfg.no_trio else [])
    outputs: list[str] = []
    for no_trio in modes:
        argv = [sys.executable, str(PROC_DIFF), "--odin3", cfg.odin3, "--yosys", cfg.yosys,
                "--mem-cap", str(cap), *(["--top", top] if top else []),
                *(["--techlib", cfg.techlib] if cfg.techlib else []),
                *(["--nolatches"] if cfg.nolatches else []), *(["--no-trio"] if no_trio else []),
                str(item.path)]
        try:  # proc-diff caps each of its own children
            done = subprocess.run(argv, capture_output=True, text=True, errors="replace",
                                  timeout=TIMEOUT_S, check=False, pass_fds=lock_fds())
        except subprocess.TimeoutExpired:
            return Outcome("fail", time.monotonic() - start, f"timed out after {TIMEOUT_S} s",
                           hard=True)
        outcome = proc_diff_outcome(done, time.monotonic() - start)
        if outcome.kind != "ok":
            return outcome
        outputs.append(outcome.output)
    return Outcome("ok", time.monotonic() - start, output="".join(outputs))


def run_opt_tool(cfg: Config, item: Item, cap: int) -> Outcome:
    """opt-equiv, opt-diff or opt-run on the file: exit 0 passes, 1 is a difference, 2 an error,
    and 3 (the one convention of the opt tools) a crash, cap kill or timeout of a child: hard."""
    start = time.monotonic()
    assert cfg.yosys is not None or cfg.opt_tool == "opt-run"
    top = top_for(item, cfg.tops, cfg.golden_root)
    argv = [sys.executable, str(OPT_TOOLS[cfg.opt_tool]), "--odin3", cfg.odin3,
            *(["--yosys", cfg.yosys] if cfg.yosys else []), "--mem-cap", str(cap),
            *(["--top", top] if top else []),
            *(["--techlib", cfg.techlib] if cfg.techlib else []), *cfg.opt_args, str(item.path)]
    here = str(Path(__file__).resolve().parent)  # the tool imports memcap from here
    env = {**os.environ, "PYTHONPATH": os.pathsep.join([here, os.environ.get("PYTHONPATH", "")])}
    try:  # the tool caps each of its own children
        done = subprocess.run(argv, capture_output=True, text=True, errors="replace",
                              timeout=TIMEOUT_S, check=False, pass_fds=lock_fds(), env=env)
    except subprocess.TimeoutExpired:
        return Outcome("fail", time.monotonic() - start, f"timed out after {TIMEOUT_S} s",
                       hard=True)
    secs = time.monotonic() - start
    text = (done.stdout or "") + (done.stderr or "")
    if done.returncode == 0:
        return Outcome("ok", secs, output=done.stdout or "")
    if done.returncode == EXIT_SKIP:
        return Outcome("skip", secs, "cannot be capped")
    why = "NOT ok: " if done.returncode == 1 else ""
    return Outcome("fail", secs, why + (first_error(text) or f"exit {done.returncode}"), text,
                   hard=done.returncode < 0 or done.returncode == OPT_CRASH
                   or any(mark in text for mark in OPT_TOOL_ABORT))


def run_equiv(cfg: Config, item: Item, cap: int) -> Outcome:
    start = time.monotonic()
    assert cfg.yosys is not None and cfg.equiv_check is not None
    top = top_for(item, cfg.tops, cfg.golden_root)
    argv = [sys.executable, str(ELAB_EQUIV), "--odin3", cfg.odin3, "--yosys", cfg.yosys,
            "--equiv-check", cfg.equiv_check, "--mem-cap", str(cap), "--unmap-enables",
            *(["--top", top] if top else []), *(["--techlib", cfg.techlib] if cfg.techlib else []),
            *[arg for lib in cfg.libs for arg in ("--lib", lib)],
            *(["--lib-nomem2reg"] if cfg.libs else []),
            *(["--vtr-compat"] if cfg.vtr_compat else []), *proc_flags(cfg), str(item.path)]
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
    return Outcome("fail", secs, first_error(text) or f"exit {code}", text,
                   hard=code < 0 or any(mark in text for mark in BRIDGE_HARD))


def run_file(cfg: Config, item: Item) -> Outcome:
    cap = cfg.cap_of(item)
    mode = cfg.mode()
    if mode == "bench":
        return run_bench(cfg, item, cap)
    if mode == "procdiff":
        return run_proc_diff(cfg, item, cap)
    if mode == "opt":
        return run_opt_tool(cfg, item, cap)
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
        self.changed_total = 0  # opt-equiv: the calls that changed something, over the ok files
        self.max_iterations = (0, "")  # opt-run: the largest `opt` iteration count and its file

    def note_iterations(self, item: Item, output: str) -> None:
        found = OPT_RUN_ITERATIONS.search(output)
        if found is not None and int(found.group(1)) > self.max_iterations[0]:
            self.max_iterations = (int(found.group(1)), item.rel)

    def report(self, item: Item, outcome: Outcome) -> None:
        cls, line = verdict(item, outcome, self.expect, self.xl)
        print(line, flush=True)
        if outcome.kind == "ok" and self.cfg.mode() == "bench" and outcome.output:
            print(f"bench: {outcome.output}", flush=True)
            self.bench_rows[item.rel] = parse_bench(outcome.output)
        if outcome.kind == "ok" and self.cfg.opt_tool == "opt-equiv":
            self.changed_total += sum(int(n) for n in OPT_CHANGED_CALLS.findall(outcome.output))
        if outcome.kind == "ok" and self.cfg.opt_tool == "opt-run":
            self.note_iterations(item, outcome.output)
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
    if cfg.opt_tool == "opt-run":
        print(f"{TOOL}: opt-run: maximum opt iterations {state.max_iterations[0]} "
              f"({state.max_iterations[1] or 'none'}; the cap is 10000)")
    if state.skipped_run and not counts["ok"] + counts["xfail"]:
        return EXIT_SKIP
    short = cfg.opt_tool == "opt-equiv" and state.changed_total < args.min_changed_total
    if short:  # an odin3 that does nothing passes every per-file check (2F-6 re-review N3)
        print(f"{TOOL}: FAIL: opt-equiv: {state.changed_total} changing calls in total, want at "
              f"least {args.min_changed_total}")
    missed = False
    if cfg.bench_report:
        lines, missed = bench_report.report(cfg.bench_report, state.bench_rows,
                                            frozenset(expect))
        print("\n".join(lines))
    return EXIT_FAIL if counts["FAIL"] or counts["XPASS"] or stale or missed or short else EXIT_OK


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
                    help="after a --bench sweep, print the targets of this profile (elab, proc)")
    ap.add_argument("--proc-diff", action="store_true", dest="proc_diff",
                    help="run proc-diff (Odin III's proc against Yosys 0.55's) per file")
    ap.add_argument("--opt-equiv", action="store_true", dest="opt_equiv",
                    help="run opt-equiv (refinement after every opt call) per file")
    ap.add_argument("--opt-diff", action="store_true", dest="opt_diff",
                    help="run opt-diff (Odin III's opt against Yosys 0.55's) per file")
    ap.add_argument("--opt-run", action="store_true", dest="opt_run",
                    help="run opt-run (Phase 2 script, ODIN3_OK, check, iteration cap) per file")
    ap.add_argument("--opt-arg", action="append", default=[], metavar="ARG",
                    help="--opt-equiv/--opt-diff: an argument for the child (use --opt-arg=ARG)")
    ap.add_argument("--min-changed-total", type=int, default=0, metavar="N",
                    help="--opt-equiv: fail (exit 1) unless at least N calls changed something "
                         "over the whole sweep (a do-nothing odin3 passes every file)")
    ap.add_argument("--pass", choices=("elab", "proc"), default="elab", dest="stop_pass",
                    help="--equiv: side A stops after elaboration (default) or after proc")
    ap.add_argument("--trio", choices=("auto", "on", "off"), default="auto",
                    help="proc modes: the input after the pre-proc passes (auto: once opt exists)")
    ap.add_argument("--also-no-trio", action="store_true",
                    help="--proc-diff: run each file again without the pre-proc passes")
    ap.add_argument("--vtr-compat", action="store_true",
                    help="--equiv: elab-equiv --vtr-compat (the VTR flow's setting)")
    ap.add_argument("--nolatches", action="store_true",
                    help="--proc-diff: read_verilog --nolatches")
    ap.add_argument("--expect-fail", type=Path, action="append", default=[],
                    help="<relative path> <reason> lines (repeatable)")
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--mem-cap", default="4G", help="per child (default 4G)")
    ap.add_argument("--cap-for", action="append", default=[], metavar="SUFFIX=CAP",
                    help="a larger cap for one file, which then runs alone (e.g. koios/lenet.v=5G)")
    ap.add_argument("--techlib", type=Path,
                    help="read this tech library (.o3lib) before each file: the hard blocks "
                         "(adder, multiply, single/dual_port_ram) the micros instantiate")
    ap.add_argument("--lib", type=Path, action="append", default=[], metavar="FILE",
                    help="read FILE as library units (read_verilog --lib --nomem2reg) before each "
                         "file: Parmys's vtr_primitives.v, for the micros' `adder #(W)`")
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
    modes = [args.equiv, args.bench, args.proc_diff, args.opt_equiv, args.opt_diff, args.opt_run]
    if sum(bool(m) for m in modes) > 1:
        raise UsageError("--equiv, --bench, --proc-diff, --opt-equiv, --opt-diff and --opt-run "
                         "cannot be combined")
    if args.min_changed_total and not args.opt_equiv:
        raise UsageError("--min-changed-total needs --opt-equiv")
    if (args.opt_equiv or args.opt_diff) and not args.yosys:
        raise UsageError("--opt-equiv and --opt-diff need --yosys")
    if args.bench_report and not args.bench:
        raise UsageError("--bench-report needs --bench")
    if args.bench_report and args.bench_report not in bench_report.PROFILES:
        raise UsageError(f"unknown bench profile {args.bench_report!r} "
                         f"(known: {', '.join(bench_report.PROFILES)})")
    if args.equiv and not (args.yosys and args.equiv_check):
        raise UsageError("--equiv needs --yosys and --equiv-check")
    if args.proc_diff and not args.yosys:
        raise UsageError("--proc-diff needs --yosys")
    if args.stop_pass == "proc" and not args.equiv:
        raise UsageError("--pass proc needs --equiv")
    caps: dict[str, int] = {}
    for spec in args.cap_for:
        suffix, _, size = spec.partition("=")
        if not suffix or not size:
            raise UsageError(f"--cap-for {spec!r}: expected SUFFIX=CAP")
        caps[suffix] = parse_size(size)
    cap = parse_size(args.mem_cap)
    yosys_mode = args.equiv or args.proc_diff or args.opt_equiv or args.opt_diff
    for tool in (args.odin3, args.bench) if not yosys_mode else (args.odin3,):
        if tool is not None and shutil.which(tool) is None:
            raise UsageError(f"{tool} is not an executable file")
    golden = args.golden_root
    if golden is None:
        golden = load_micro_top().DEFAULT_ROOT
    tops = read_tops(args.tops) if args.tops else {}
    return Config(
        odin3=os.path.abspath(shutil.which(args.odin3) or args.odin3),
        yosys=_abs(args.yosys) if yosys_mode else None,
        equiv_check=_abs(args.equiv_check) if args.equiv else None,
        bench=_abs(args.bench) if args.bench else None, cap=cap, caps=caps, tops=tops,
        golden_root=golden, techlib=_abs(args.techlib) if args.techlib else None,
        bench_report=args.bench_report, libs=tuple(str(Path(lib).resolve()) for lib in args.lib),
        proc_diff=bool(args.proc_diff),
        opt_tool="opt-equiv" if args.opt_equiv else "opt-diff" if args.opt_diff
        else "opt-run" if args.opt_run else "",
        opt_args=tuple(args.opt_arg), pass_proc=args.stop_pass == "proc",
        no_trio=args.trio == "off", also_no_trio=bool(args.also_no_trio),
        nolatches=bool(args.nolatches), vtr_compat=bool(args.vtr_compat))


def _abs(tool: str | None) -> str | None:
    if tool is None:
        return None
    return os.path.abspath(shutil.which(tool) or tool)


def settle_trio(cfg: Config, choice: str) -> Config:
    """``--trio auto`` in a proc mode: the input after the pre-proc passes once ``opt`` exists,
    else ``--no-trio`` with a notice; ``on`` requires it. Elsewhere the config stays."""
    if not (cfg.proc_diff or cfg.pass_proc) or choice == "off":
        return cfg
    done = memcap.run([cfg.odin3, "-p", TRIO_PROBE], cfg.cap, capture_output=True)
    if done.returncode == 0:
        return cfg
    if choice == "on":
        raise UsageError(f"--trio on: `{TRIO_PROBE}` does not run (exit {done.returncode})")
    reason = ((done.stdout or "") + (done.stderr or "")).strip().splitlines()[-1:] or ["no output"]
    print(f"{TOOL}: trio: skipped ({TRIO_PROBE} exit {done.returncode}: {reason[0]})")
    return dataclasses.replace(cfg, no_trio=True)


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
        cfg = settle_trio(cfg, args.trio)
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
