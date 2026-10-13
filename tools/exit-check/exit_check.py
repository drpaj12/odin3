"""exit-check: the Phase 2 exit-test harness (2H; PHASE2 #4 and decision 13's exit-bar ruling).

Usage::

    exit-check --odin3 O --yosys Y --arch A [--sp-splice S] [--netlist-compare N]
               [--equiv-check E] [--golden-root D] [--techlib L] [--primitives P]
               [--vtr-config-template X | --config C | --no-config]
               [--expect-fail F]... [--jobs J] [--mem-cap C] [--cap-for SUFFIX=C]...
               [--equiv-timeout S] [--work DIR] [--root R] [--limit N] [--partial]
               <dir or file>...

Every ``.v`` under the directories (and the files) given is one design, run **smallest first** (all
together, ties by relative path; ``--limit N`` keeps the N smallest that have a Parmys golden). For
each design:

1. its Parmys golden is found: ``D/regression/verilog/<group>/<leaf>/<leaf>.parmys.blif`` for a
   micro under ``micro/verilog`` (``micro-top``'s rule), ``D/vtr/<name>/<name>.parmys.blif`` for a
   VTR benchmark under ``benchmarks/verilog``; ``D`` defaults to the k6_frac tree of
   ``micro-top``. A design whose golden does not exist and whose oracle run left no ``.prov`` at
   all (the oracle never ran on it: the 41 Koios designs have no k6_frac golden) is **skipped (no
   golden)**, counted apart and no failure; one with a ``.prov`` (Parmys rejected it, or it was
   capped) is an ``error``;
2. ``sp-splice`` (Odin III's front end through ``opt`` and ``proc``, then Yosys 0.55 with Parmys
   for the rest of VTR's script) runs with the same ``--arch`` and ``--top`` (the golden's first
   ``.model``) and the same Parmys config as the goldens' run (VTR's
   ``basic_odin_config_split.xml``, filled per design by ``write_parmys_config``: a fixed hard
   ``multiply``, memories split to the arch's address width, min multiplier size 3, adder
   threshold 1); it caps its own children at ``--mem-cap``;
3. ``netlist-compare --implicit-models`` golden vs output (Parmys leaves a ``$pow`` and the like
   as a ``.subckt`` with no model): exit 0 is ``identical``;
4. else ``equiv-check --x-report --match-unnamed`` (Parmys writes no ``.cname`` for a hard block
   and numbers its output nets with counters, ``$mul~14[0]``, so equiv-check pairs the blocks by
   simulation signature, not by name) golden vs output: exit 0 is ``equivalent``, exit 3
   ``equivalent outside x-cones`` (``eq-x``; the removed outputs are listed), exit 1 ``differs``,
   anything else ``error``. Each of these children runs under ``memcap.run``.

Decision 13: ``identical`` and ``equivalent`` count toward the exit bar. ``eq-x`` does not unless
the design is listed (its excluded outputs reviewed and recorded in the exceptions list);
``differs`` and ``error`` never do unless listed.

``--expect-fail F`` (repeatable) is elab-sweep's list, parsed by it: ``<relative path> [needle]
<reason>``; the path is relative to ``--root`` (else the directory argument, else the file name).
A listed design must fail to reach ``identical``/``equivalent`` and its log must hold the needle.
An unlisted ``eq-x``, ``differs`` or ``error``, a listed design that now passes (XPASS), a needle
that does not match, and (without ``--partial``) a listed path that names no design fail the run.
A crash, a cap kill (out of memory) and a timeout are **never** expected results, listed or not.

Output: one line per design as it finishes, then a summary table, every cause that is not a pass,
and a totals line. ``--work DIR`` (else a fresh temporary directory, printed and left in place)
holds ``NNNN-<design>/exit-check.log`` per design with every child's output, beside the splice's
own files. Nothing is deleted. ``--jobs J`` runs J designs at once, refused when J times the cap
exceeds 60% of MemTotal or the heavy slice; a design with a ``--cap-for`` cap larger than
``--mem-cap`` runs alone.

Exit codes: 0 every design identical, equivalent or listed; 1 anything else; 2 usage or input
error; 77 skipped (an ASan odin3 and no way to cap it).
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from collections.abc import Sequence
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "elab-check"))
import elab_sweep  # noqa: E402
import memcap  # noqa: E402
from elab_equiv import format_cap  # noqa: E402

TOOL = "exit-check"
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_SKIP = 0, 1, 2, 77
IDENTICAL, EQUIVALENT, EQ_X, DIFFERS, ERROR, SKIP, NO_GOLDEN = (
    "identical", "equivalent", "eq-x", "differs", "error", "skip", "no-golden")
PASSING = (IDENTICAL, EQUIVALENT)
CLASS_ORDER = (IDENTICAL, EQUIVALENT, EQ_X, DIFFERS, ERROR)
TIMEOUT_S = elab_sweep.TIMEOUT_S
EQUIV_EXIT_XCONE = 3  # equiv-check --x-report: equivalent outside the x-cones
SPLICE_EXIT_SKIP = 77
LOG_LINES = 60  # lines of one child's output kept in a design's log
WHY_WIDTH = 160
MICRO_MARK = ("micro", "verilog")
VTR_MARK = ("benchmarks", "verilog")
_SAFE = re.compile(r"[^A-Za-z0-9_.-]+")
# sp-splice reports a child that died of a signal or the cap as "failed (exit -9)" / "(exit 137)".
_SIGNALLED = re.compile(r"\(exit (-\d+|137)\)")
_PATH = re.compile(r"(?:/[^/\s:]+)+/([^/\s:]+)")
_NOT_EQUIV = re.compile(r"^equiv-check: (NOT equivalent.*|x-report: .*)$", re.MULTILINE)


class UsageError(elab_sweep.UsageError):
    """A bad argument or input file (exit code 2)."""


@dataclass(frozen=True)
class Result:
    kind: str  # identical, equivalent, eq-x, differs, error or skip
    secs: float = 0.0
    why: str = ""
    output: str = ""  # the design's log: the needle of an expect-fail entry must appear in it
    hard: bool = False  # a signal, a cap kill, a timeout or a child that cannot run
    cones: tuple[str, ...] = ()  # eq-x: the outputs removed


@dataclass(frozen=True)
class Config:
    odin3: str
    yosys: str
    arch: str
    sp_splice: str
    netlist_compare: str
    equiv_check: str
    golden_root: Path
    work: Path
    cap: int
    caps: dict[str, int]
    techlib: str | None = None
    primitives: str | None = None
    config: str | None = None
    config_template: Path | None = None  # VTR's basic_odin_config_split.xml, filled per design
    min_mult: int = 3
    min_adder: int = 1
    equiv_timeout: int = 600

    def cap_of(self, item: elab_sweep.Item) -> int:
        for suffix, cap in self.caps.items():
            if item.rel == suffix or item.rel.endswith("/" + suffix):
                return cap
        return self.cap


# ---- Parmys's config -------------------------------------------------------------------------

TEMPLATE_UNDER_VTR = "vtr_flow/misc/basic_odin_config_split.xml"


def memory_addr_width(arch: Path) -> int:
    """The largest RAM address width in the architecture (VTR's determine_memory_addr_width):
    the ``addr`` input ports of the ``*port_ram`` primitives."""
    width = 0
    for elem in ET.parse(arch).getroot().iter("pb_type"):
        if "port_ram" in (elem.get("blif_model") or ""):
            for port in elem.findall("input"):
                if "addr" in (port.get("name") or ""):
                    width = max(width, int(port.get("num_pins") or 0))
    return width


def write_parmys_config(cfg: Config, source: Path, work: Path) -> Path:
    """The ``-c`` file of VTR's Parmys stage (``basic_odin_config_split.xml`` with its
    placeholders filled as ``vtr/parmys/parmys.py`` fills them): the hard multiplier is fixed at
    the architecture's ``multiply`` (``fixed="1"``), the memories split to the architecture's
    address width, ``-min_hard_mult_size`` 3 and ``-min_hard_adder_size`` 1 (run_vtr_flow's
    defaults, which the goldens used)."""
    assert cfg.config_template is not None
    text = cfg.config_template.read_text(encoding="utf-8")
    fills = {"YYY": Path(cfg.arch).name, "TTT": "Verilog", "ZZZ": "parmys.out.blif",
             "PPP": str(memory_addr_width(Path(cfg.arch))), "MMM": str(cfg.min_mult),
             "AAA": str(cfg.min_adder), "XXX": str(source)}
    for key, value in fills.items():
        text = text.replace(key, value)
    work.mkdir(parents=True, exist_ok=True)
    out = work / "parmys_config.xml"
    out.write_text(text, encoding="utf-8")
    return out


# ---- goldens ----------------------------------------------------------------------------------


def _after(parts: tuple[str, ...], mark: tuple[str, ...]) -> tuple[str, ...] | None:
    """The path components after the last occurrence of ``mark`` in ``parts``, else None."""
    for i in range(len(parts) - len(mark), -1, -1):
        if parts[i:i + len(mark)] == mark:
            return parts[i + len(mark):]
    return None


def golden_path(root: Path, source: Path) -> Path:
    """The Parmys golden BLIF of ``source``: a VTR benchmark's is ``vtr/<name>/<leaf>.parmys.blif``
    (``<name>`` keeps a ``koios/`` directory), a micro's is ``micro-top``'s."""
    parts = source.resolve().parts
    vtr = _after(parts, VTR_MARK)
    if vtr:
        name = Path(*vtr).with_suffix("")
        return root / "vtr" / name / f"{name.name}.parmys.blif"
    return Path(elab_sweep.load_micro_top().golden_path(root, str(source)))


def golden_problem(golden: Path) -> str:
    """Why ``golden`` cannot be used (empty when it can): the oracle's recorded status."""
    if golden.is_file():
        return ""
    prov = golden.with_suffix(".prov")
    if prov.is_file():
        for line in prov.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("status="):
                status = line.partition("=")[2]
                verb = "Parmys rejected the design" if status == "failed" else \
                    f"Parmys run {status} (no verdict)"
                return f"no Parmys golden: {verb} (status={status})"
    return f"no Parmys golden: {golden} does not exist"


# ---- running the children ---------------------------------------------------------------------


def clip(text: str, lines: int = LOG_LINES) -> str:
    rows = text.rstrip().splitlines()
    if len(rows) <= lines:
        return "\n".join(rows)
    return "\n".join([*rows[:lines], f"... ({len(rows) - lines} more lines)"])


def one_line(text: str) -> str:
    """One line of at most WHY_WIDTH characters, every absolute path cut to its file name."""
    line = _PATH.sub(r"\1", " ".join(text.split()))
    return line if len(line) <= WHY_WIDTH else line[:WHY_WIDTH - 3] + "..."


def run_capped(argv: Sequence[str], cap: int, log: list[str]) -> subprocess.CompletedProcess[str] \
        | Result:
    """A child under ``memcap.run``; its command line goes to ``log``. A child that cannot run or
    times out is a hard error Result."""
    log.append("$ " + " ".join(argv))
    try:
        return memcap.run(list(argv), cap, capture_output=True, timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return Result(ERROR, why=f"timed out after {TIMEOUT_S} s", hard=True)
    except OSError as exc:
        return Result(ERROR, why=f"cannot run {argv[0]}: {exc}", hard=True)


def splice_why(done: subprocess.CompletedProcess[str], cap: int) -> tuple[str, bool]:
    """(why, hard) of a failed sp-splice: its first message line; a signal or the cap is hard."""
    text = (done.stdout or "") + (done.stderr or "")
    message = elab_sweep.first_error(text.replace("sp-splice: error: ", ""))
    hard = done.returncode < 0 or _SIGNALLED.search(text) is not None
    if hard and re.search(r"\(exit (-9|137)\)", text):
        return f"out of memory (cap {format_cap(cap)}): {message}", True
    return f"sp-splice: {message}", hard


def splice_argv(cfg: Config, item: elab_sweep.Item, top: str, work: Path,
                config: str | None) -> list[str]:
    argv = [cfg.sp_splice, "--odin3", cfg.odin3, "--yosys", cfg.yosys, "--arch", cfg.arch,
            "--top", top, "--mem-cap", str(cfg.cap_of(item)), "--keep", str(work)]
    for flag, value in (("--techlib", cfg.techlib), ("--primitives", cfg.primitives),
                        ("--config", config)):
        if value:
            argv += [flag, value]
    return [*argv, str(item.path)]


def run_splice(cfg: Config, item: elab_sweep.Item, top: str, work: Path,
               log: list[str]) -> Path | Result:
    """sp-splice on the design: the output BLIF, or the Result of its failure. sp-splice caps its
    own children at the design's cap (it is one Python process and needs none itself)."""
    config = cfg.config
    if cfg.config_template is not None:
        config = str(write_parmys_config(cfg, item.path, work))
    argv = splice_argv(cfg, item, top, work, config)
    log.append("$ " + " ".join(argv))
    try:
        done = subprocess.run(argv, capture_output=True, text=True, errors="replace",
                              timeout=TIMEOUT_S, check=False, pass_fds=elab_sweep.lock_fds())
    except subprocess.TimeoutExpired:
        return Result(ERROR, why=f"timed out after {TIMEOUT_S} s", hard=True)
    except OSError as exc:
        return Result(ERROR, why=f"cannot run {argv[0]}: {exc}", hard=True)
    log.append(clip((done.stdout or "") + (done.stderr or "")))
    if done.returncode == SPLICE_EXIT_SKIP:
        return Result(SKIP, why="cannot be capped")
    if done.returncode != 0:
        why, hard = splice_why(done, cfg.cap_of(item))
        return Result(ERROR, why=why, hard=hard)
    lines = (done.stdout or "").strip().splitlines()
    out = Path(lines[-1]) if lines else work / "out.blif"
    if not out.is_file():
        return Result(ERROR, why=f"sp-splice wrote no {out}")
    return out


def compare_failure(done: subprocess.CompletedProcess[str], cap: int, what: str) -> Result:
    """The error Result of a comparison child that exited with a code it does not define."""
    text = "\n".join(line.removeprefix(f"{what}: ")
                     for line in ((done.stdout or "") + (done.stderr or "")).splitlines()
                     if not line.startswith(f"{what}: notice:"))
    hard = done.returncode < 0 or done.returncode == 137
    return Result(ERROR, why=(elab_sweep.child_why(done, cap) if hard
                              else f"{what}: " + one_line(elab_sweep.first_error(text))),
                  hard=hard)


def differs_why(text: str) -> str:
    """The reason line(s) of an equiv-check exit 1."""
    marks = [m.group(1) for m in _NOT_EQUIV.finditer(text)]
    return one_line("; ".join(marks)) if marks else one_line(elab_sweep.first_error(text))


def cone_names(text: str) -> tuple[str, ...]:
    return tuple(line.partition(":")[2].strip() for line in text.splitlines()
                 if line.startswith("x-cone:"))


def run_equiv(cfg: Config, golden: Path, out: Path, cap: int, log: list[str]) -> Result:
    """``equiv-check --x-report golden out`` classified."""
    got = run_capped([cfg.equiv_check, "--x-report", "--match-unnamed", "--timeout",
                      str(cfg.equiv_timeout), str(golden), str(out)], cap, log)
    if isinstance(got, Result):
        return got
    text = (got.stdout or "") + (got.stderr or "")
    log.append(clip(text))
    code = got.returncode
    if code == 0:
        return Result(EQUIVALENT)
    if code == EQUIV_EXIT_XCONE:
        cones = cone_names(text)
        return Result(EQ_X, why=f"{len(cones)} output(s) in x-cones: " + one_line(
            ", ".join(cones)), cones=cones)
    if code == 1:
        return Result(DIFFERS, why=differs_why(text))
    inconclusive = re.search(r"^equiv-check: (inconclusive: .*)$", text, re.MULTILINE)
    if code == 2 and inconclusive:  # a guessed black-box pairing failed: no verdict either way
        return Result(ERROR, why=one_line(inconclusive.group(1)))
    return compare_failure(got, cap, "equiv-check")


def classify(cfg: Config, golden: Path, out: Path, cap: int, log: list[str]) -> Result:
    """netlist-compare, then equiv-check --x-report. A netlist-compare error is logged and the
    equivalence check decides."""
    got = run_capped([cfg.netlist_compare, "--implicit-models", str(golden), str(out)], cap, log)
    if isinstance(got, Result):
        return got
    log.append(clip((got.stdout or "") + (got.stderr or "")))
    if got.returncode == 0:
        return Result(IDENTICAL)
    if got.returncode < 0 or got.returncode == 137:
        return compare_failure(got, cap, "netlist-compare")
    if got.returncode != 1:
        log.append(f"netlist-compare exit {got.returncode}: not a verdict; equiv-check decides")
    return run_equiv(cfg, golden, out, cap, log)


def design_dir(cfg: Config, index: int, item: elab_sweep.Item) -> Path:
    return cfg.work / f"{index:04d}-{_SAFE.sub('_', item.rel)}"


def pipeline(cfg: Config, item: elab_sweep.Item, work: Path, log: list[str]) -> Result:
    golden = golden_path(cfg.golden_root, item.path)
    problem = golden_problem(golden)
    if problem:
        if not golden.with_suffix(".prov").is_file():  # the oracle never ran on it
            return Result(NO_GOLDEN, why=f"{golden.name} does not exist")
        return Result(ERROR, why=problem)
    try:
        top = str(elab_sweep.load_micro_top().first_model(golden))
    except (OSError, ValueError) as exc:
        return Result(ERROR, why=f"golden has no top: {exc}")
    log.append(f"golden {golden}\ntop {top}")
    work.mkdir(parents=True, exist_ok=True)
    out = run_splice(cfg, item, top, work, log)
    if isinstance(out, Result):
        return out
    return classify(cfg, golden, out, cfg.cap_of(item), log)


def run_design(cfg: Config, item: elab_sweep.Item, index: int) -> Result:
    """One design through the pipeline; its log is written to ``NNNN-<design>/exit-check.log``."""
    start = time.monotonic()
    work = design_dir(cfg, index, item)
    log = [f"design {item.rel} ({item.size} B)"]
    result = pipeline(cfg, item, work, log)
    log.append(f"result {result.kind}: {result.why}".rstrip(": "))
    text = "\n".join(log)
    try:
        work.mkdir(parents=True, exist_ok=True)
        (work / "exit-check.log").write_text(text + "\n", encoding="utf-8")
    except OSError as exc:
        text += f"\n(cannot write the log: {exc})"
    return dataclasses.replace(result, secs=time.monotonic() - start, output=text)


# ---- verdicts and the summary -----------------------------------------------------------------


def verdict(item: elab_sweep.Item, result: Result,
            expect: dict[str, elab_sweep.Expect]) -> tuple[str, str]:
    """(class, line): class is a result kind, ``listed``, ``XPASS``, ``FAIL`` or ``skip``."""
    head = f"{item.rel} ({item.size} B, {result.secs:.3f} s)"
    entry = expect.get(item.rel)
    if result.kind == SKIP:
        return "skip", f"skip       {head}: {result.why}"
    if result.kind == NO_GOLDEN:
        return "nogolden", f"skipped    {head}: no golden: {result.why}"
    if entry is None:
        return result.kind, f"{result.kind:<10} {head}" + (f": {result.why}" if result.why
                                                          else "")
    if result.kind in PASSING:
        return "XPASS", (f"XPASS      {head}: listed as an exception ({entry.reason}) but is "
                         f"{result.kind}")
    if result.hard:  # a crash, cap kill or timeout is never the listed result
        return "hard", (f"{result.kind:<10} {head}: {result.why} (listed, but a signal, a memory "
                        "cap or a timeout is never an expected result)")
    if entry.needle and entry.needle not in result.output:
        return "needle", (f"{result.kind:<10} {head}: fails differently from the listing: "
                          f"expected [{entry.needle}], got {result.why}")
    return "listed", f"listed     {head}: {result.kind}: {entry.reason}"


def unexpected(cls: str) -> bool:
    return cls in (EQ_X, DIFFERS, ERROR, "XPASS", "hard", "needle")


class Sweep:
    def __init__(self, cfg: Config, expect: dict[str, elab_sweep.Expect], jobs: int) -> None:
        self.cfg = cfg
        self.expect = expect
        self.jobs = jobs
        self.counts: dict[str, int] = {}
        self.listed: dict[str, int] = {}
        self.causes: list[str] = []
        self.skipped = False

    def report(self, item: elab_sweep.Item, result: Result) -> None:
        cls, line = verdict(item, result, self.expect)
        print(line, flush=True)
        self.counts[cls] = self.counts.get(cls, 0) + 1
        if cls == "listed":
            self.listed[result.kind] = self.listed.get(result.kind, 0) + 1
        if cls == "skip":
            self.skipped = True
        if result.kind in (EQ_X, DIFFERS, ERROR) and cls != "skip":
            self.causes.append(f"{cls if cls != result.kind else 'unlisted'} {result.kind}: "
                               f"{item.rel}: {result.why}")

    def run(self, items: Sequence[elab_sweep.Item]) -> None:
        pending: list[tuple[elab_sweep.Item, Future[Result]]] = []
        with ThreadPoolExecutor(max_workers=self.jobs) as pool:
            for index, item in enumerate(items):
                if self.cfg.cap_of(item) > self.cfg.cap:  # a raised cap runs alone
                    self.drain(pending, 0)
                    self.report(item, run_design(self.cfg, item, index))
                    continue
                pending.append((item, pool.submit(run_design, self.cfg, item, index)))
                self.drain(pending, self.jobs)
            self.drain(pending, 0)

    def drain(self, pending: list[tuple[elab_sweep.Item, Future[Result]]], keep: int) -> None:
        while len(pending) > keep:
            item, future = pending.pop(0)
            self.report(item, future.result())


def summary_lines(state: Sweep, total: int, stale: Sequence[str], secs: float) -> list[str]:
    counts = state.counts
    listed = sum(state.listed.values())
    lines = ["", f"{TOOL}: summary ({total} designs)",
             f"  {'class':<24}{'designs':>8}", f"  {'-' * 32}"]
    names = {IDENTICAL: "identical", EQUIVALENT: "equivalent", EQ_X: "equivalent outside x",
             DIFFERS: "differs", ERROR: "error"}
    lines += [f"  {names[kind]:<24}{counts.get(kind, 0):>8}" for kind in CLASS_ORDER]
    detail = ", ".join(f"{n} {kind}" for kind, n in sorted(state.listed.items()))
    lines.append(f"  {'listed' + (' (' + detail + ')' if detail else ''):<24}{listed:>8}")
    for extra, label in (("XPASS", "listed but passing"), ("hard", "listed, hard failure"),
                         ("needle", "listed, wrong needle"), ("skip", "skipped"),
                         ("nogolden", "skipped (no golden)")):
        if counts.get(extra):
            lines.append(f"  {label:<24}{counts[extra]:>8}")
    if stale:
        lines.append(f"  {'stale entries':<24}{len(stale):>8}")
    lines += [f"  {line}" for line in state.causes] if state.causes else []
    lines.append(f"{TOOL}: {total} designs: {counts.get(IDENTICAL, 0)} identical, "
                 f"{counts.get(EQUIVALENT, 0)} equivalent, {counts.get(EQ_X, 0)} equivalent "
                 f"outside x-cones, {counts.get(DIFFERS, 0)} differs, {counts.get(ERROR, 0)} "
                 f"error, {listed} listed, {counts.get('XPASS', 0)} listed but passing, "
                 f"{len(stale)} stale, {counts.get('nogolden', 0)} skipped (no golden), "
                 f"{secs:.1f} s")
    return lines


def failed(state: Sweep, stale: Sequence[str]) -> bool:
    return bool(stale) or any(unexpected(cls) and n for cls, n in state.counts.items())


def sweep(cfg: Config, items: list[elab_sweep.Item], expect: dict[str, elab_sweep.Expect],
          jobs: int, partial: bool) -> int:
    state = Sweep(cfg, expect, jobs)
    start = time.monotonic()
    print(f"{TOOL}: work directory {cfg.work}", flush=True)
    state.run(items)
    seen = {item.rel for item in items}
    stale = [] if partial else sorted(rel for rel in expect if rel not in seen)
    for rel in stale:
        print(f"STALE {rel}: listed as an exception but not in the corpus")
    lines = summary_lines(state, len(items), stale, time.monotonic() - start)
    print("\n".join(lines))
    (cfg.work / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    if state.skipped and not sum(state.counts.get(k, 0) for k in (*CLASS_ORDER, "listed")):
        return EXIT_SKIP
    return EXIT_FAIL if failed(state, stale) else EXIT_OK


# ---- arguments --------------------------------------------------------------------------------


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    tools = HERE.parent
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary (Release for a sweep)")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--arch", required=True, help="the architecture file the goldens used")
    ap.add_argument("--sp-splice", default=str(tools / "opt-check" / "sp-splice"))
    ap.add_argument("--netlist-compare", default=str(tools / "netlist-compare" / "netlist-compare"))
    ap.add_argument("--equiv-check", default=str(tools / "equiv-check" / "equiv-check"))
    ap.add_argument("--golden-root", type=Path, default=None,
                    help="the Parmys golden tree of the arch (default: micro-top's)")
    ap.add_argument("--techlib", help="sp-splice --techlib (default: its own lib/vtr.o3lib)")
    ap.add_argument("--primitives", help="sp-splice --primitives (default under $VTR_ROOT)")
    ap.add_argument("--config", help="sp-splice --config: one Parmys config file for every design "
                    "(instead of VTR's, filled per design)")
    ap.add_argument("--vtr-config-template", type=Path, metavar="FILE",
                    help="VTR's basic_odin_config_split.xml, filled per design as VTR's Parmys "
                    "stage fills it (default: under $VTR_ROOT)")
    ap.add_argument("--no-config", action="store_true",
                    help="pass no Parmys config (the goldens were made with VTR's)")
    ap.add_argument("--min-hard-mult-size", type=int, default=3,
                    help="run_vtr_flow's -min_hard_mult_size (default 3, as the goldens)")
    ap.add_argument("--min-hard-adder-size", type=int, default=1,
                    help="run_vtr_flow's -min_hard_adder_size (default 1, as the goldens)")
    ap.add_argument("--expect-fail", type=Path, action="append", default=[],
                    help="<relative path> [needle] <reason> lines (repeatable)")
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--mem-cap", default="4G", help="per child (default 4G)")
    ap.add_argument("--cap-for", action="append", default=[], metavar="SUFFIX=CAP",
                    help="a larger cap for one design, which then runs alone")
    ap.add_argument("--equiv-timeout", type=int, default=600, metavar="S",
                    help="equiv-check's ABC timeout in seconds (default 600)")
    ap.add_argument("--work", type=Path, help="the work directory (default: a fresh temp dir)")
    ap.add_argument("--root", type=Path, help="make listed paths relative to this directory")
    ap.add_argument("--limit", type=int, metavar="N",
                    help="run only the N smallest designs that have a Parmys golden (a smoke run)")
    ap.add_argument("--partial", action="store_true", help="a subset: skip the stale-entry check")
    ap.add_argument("inputs", nargs="+", help="directories or files")
    return ap.parse_args(argv)


def need_exe(path: str) -> str:
    found = shutil.which(path)
    if found is None:
        raise UsageError(f"{path} is not an executable file")
    return os.path.abspath(found)


def work_dir(arg: Path | None) -> Path:
    if arg is None:
        return Path(tempfile.mkdtemp(prefix="exit-check-"))
    arg.mkdir(parents=True, exist_ok=True)
    return arg.resolve()


def config_template(args: argparse.Namespace) -> Path | None:
    """VTR's Parmys config template, unless ``--config`` or ``--no-config`` replaces it."""
    if args.no_config or args.config:
        return None
    root = os.environ.get("VTR_ROOT")
    template = args.vtr_config_template or (Path(root) / TEMPLATE_UNDER_VTR if root else None)
    if template is None or not template.is_file():
        raise UsageError("VTR's basic_odin_config_split.xml not found: set VTR_ROOT, pass "
                         "--vtr-config-template or --config, or --no-config")
    return template.resolve()


def build_config(args: argparse.Namespace) -> Config:
    caps: dict[str, int] = {}
    for spec in args.cap_for:
        suffix, _, size = spec.partition("=")
        if not suffix or not size:
            raise UsageError(f"--cap-for {spec!r}: expected SUFFIX=CAP")
        caps[suffix] = elab_sweep.parse_size(size)
    golden = args.golden_root or elab_sweep.load_micro_top().DEFAULT_ROOT
    template = config_template(args)
    arch = Path(args.arch)
    if not arch.is_file():
        raise UsageError(f"{args.arch}: no such architecture file")
    return Config(
        odin3=need_exe(args.odin3), yosys=need_exe(args.yosys), arch=str(arch.resolve()),
        sp_splice=need_exe(args.sp_splice), netlist_compare=need_exe(args.netlist_compare),
        equiv_check=need_exe(args.equiv_check), golden_root=Path(golden).resolve(),
        work=work_dir(args.work), cap=elab_sweep.parse_size(args.mem_cap), caps=caps,
        techlib=args.techlib, primitives=args.primitives, config=args.config,
        config_template=template, min_mult=args.min_hard_mult_size,
        min_adder=args.min_hard_adder_size, equiv_timeout=args.equiv_timeout)


def limit_items(cfg: Config, items: list[elab_sweep.Item], limit: int | None) \
        -> list[elab_sweep.Item]:
    """``--limit N``: the N smallest designs whose golden exists."""
    if limit is None:
        return items
    if limit < 1:
        raise UsageError(f"--limit {limit} is not positive")
    have = [item for item in items if golden_path(cfg.golden_root, item.path).is_file()]
    return have[:limit]


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        cfg = build_config(args)
        elab_sweep.check_jobs(args.jobs, cfg.cap, list(cfg.caps.values()))
        memcap.check_cap(max([cfg.cap, *cfg.caps.values()]))
        memcap.wrap([cfg.odin3], cfg.cap)  # an uncappable ASan odin3 skips before any design
        expect = elab_sweep.merge_expect(args.expect_fail)
        root = args.root.resolve() if args.root is not None else None
        items = limit_items(cfg, elab_sweep.collect(args.inputs, root), args.limit)
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP
    except (elab_sweep.UsageError, ValueError, OSError, RuntimeError) as exc:
        print(f"{TOOL}: {exc}", file=sys.stderr)
        return EXIT_USAGE
    if not items:
        print(f"{TOOL}: no input files", file=sys.stderr)
        return EXIT_USAGE
    return sweep(cfg, items, expect, args.jobs, args.limit is not None or args.partial)


if __name__ == "__main__":
    sys.exit(main())
