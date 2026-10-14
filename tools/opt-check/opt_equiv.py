"""opt-equiv: refinement after every opt sub-pass call (2F spec 20.2; plan Task 17, point (m)).

Usage::

    opt-equiv --odin3 O --yosys Y [--top T] [--script F] [--techlib L] [--no-lower]
              [--no-techmap] [--mem-cap C] [--work DIR] FILE...

Odin III runs ``read_verilog --nolatches FILE...; hierarchy; <the Phase 2 script>`` (``F``, default
``phase2.script`` beside this tool) with ``--opt-snapshot DIR``: ``NNNN-<sub>-before.il`` and
``-after.il`` for every sub-pass call that changed something. Per pair, in sequence order, Yosys
0.55 runs the refinement check of ``refinement_script``::

    read_rtlil gold.il; read_rtlil gate.il; [proc;] [<lowering>;]
    equiv_make gold gate eq; hierarchy -top eq; flatten; equiv_simple -undef -seq 4;
    equiv_induct -undef; equiv_status -assert

The modules of the two files are renamed ``gold``/``gate`` first (a submodule becomes ``gold.m`` or
``gate.m``), so one Yosys design holds both. An x in the gold is a don't-care: ``s ? a : 4'bx``
to ``a`` passes, the reverse and a wrong enable fail. A pair with ``process`` blocks (the
pre-``proc`` calls) gets Yosys ``proc`` first.

The lowering depends on the clocks (review C1). Yosys's ``async2sync`` puts every flip-flop on one
implicit clock, so it cannot see a clock net or an edge that changed. When each module has one
clock, a primary input with one polarity, with the same use on both sides (``clock_sig``), that
model is exact: the check tries ``async2sync``, then ``async2sync; techmap`` (techmap loses the
proof of a division by zero and of a ``$pmux``, which the first try gives), then ``memory;
async2sync; techmap`` (the memories). Any other pair (a clock or an edge that differs, several
clocks, a derived clock) must pass ``clk2fflogic; opt_merge; opt_clean`` (the clock becomes a
sampled signal, a flipped edge or a wrong clock net is a counterexample), also with ``techmap`` and
with ``memory`` first. A pass in any is a proof. A proof that takes past 60 s is "inconclusive" and
fails (a failing pair costs up to 3 x 60 s, each lowering has its own limit; a timeout stops the
pair). The Yosys child runs in its own process group and a timeout kills the group. ``--no-lower``
runs the plain flow (the clock is not checked), ``--no-techmap`` the first try of the family only.

A failing pair prints ``opt-equiv: <NNNN>-<sub>: not a refinement`` with the rule counters of the
run (Odin III's ``--opt-stats`` lines, which cover the whole run). A work directory this tool
made is removed after a clean run; a failing run, and ``--work DIR``, keep it.

``--min-changed N`` fails (exit 1) a run in which fewer than N calls changed something: a rule
source is built to make a rule fire, so ``--min-changed 1`` tells a broken or empty
``--opt-snapshot`` from "nothing to improve". A run with no changing call and no ``N`` prints a
``NOTE``. An empty snapshot file is an error. The work directory this tool made (under
``$TMPDIR``) is removed after a clean run, after an error (exit 2) and after a ``--min-changed``
shortfall; a pair that does not refine, and a crash (exit 3), keep it.

Exit codes (one convention across the opt tools): 0 every pair refines; 1 a pair does not; 2 usage,
Odin III or a tool error; 3 hard: a child (Odin III or Yosys) crashed, was killed by a signal or
its memory cap, or timed out (elab-sweep marks it ``hard``: never an expected failure); 77 skipped
(an ASan child and no systemd-run).
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

import clock_sig
import memcap
import toolutil

TOOL = "opt-equiv"
HERE = Path(__file__).resolve().parent
PHASE2 = HERE / "phase2.script"
EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_CRASH, EXIT_SKIP = 0, 1, 2, 3, 77
DEFAULT_CAP = 2 << 30
TIMEOUT_SECONDS = 600.0
REFINE_TIMEOUT_SECONDS = 60.0  # one equivalence proof; past it the answer is "inconclusive"
PAIR_NAME = re.compile(r"^(\d{4})-(opt_[a-z]+)-before\.il$")
STAT_LINE = re.compile(r"opt: rule (\S+) (\d+)\s*$", re.M)
MODULE_LINE = re.compile(r"^module (\\\S+)\s*$", re.M)
TOP_ATTR = re.compile(r"^attribute \\top 1\s*\n", re.M)
PROCESS_LINE = re.compile(r"^\s*process\s", re.M)
OPT_WORD = re.compile(r"^opt(_[a-z]+)?(\s|$)")


class ToolError(Exception):
    """A bad argument or an unusable tool (exit 2)."""


class CrashError(ToolError):
    """A child crashed, was killed (signal, memory cap) or timed out (exit 3, hard). ``what`` names
    the step (``odin3``, ``yosys``, ``netlist-compare``...)."""

    def __init__(self, what: str, message: str, timed_out: bool = False) -> None:
        super().__init__(f"{what}: {message}")
        self.what = what
        self.timed_out = timed_out


exe = toolutil.exe
tail = toolutil.tail
why = toolutil.why
parse_size = toolutil.parse_size


def capped(what: str, argv: Sequence[str], cap: int, *, cwd: Path,
           env: dict[str, str] | None = None, timeout: float = TIMEOUT_SECONDS
           ) -> subprocess.CompletedProcess[str]:
    """``memcap.run`` of a child; a timeout is a ``CrashError``, as is an exit that is a signal or
    a cap kill (any other exit code is the child's own verdict, for the caller to judge)."""
    try:
        done = memcap.run(list(argv), cap, capture_output=True, cwd=cwd, env=env, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        raise CrashError(what, f"timed out after {timeout:.0f} s", True) from exc
    if toolutil.is_crash(done.returncode):
        log = (done.stdout or "") + (done.stderr or "")
        raise CrashError(what, f"killed (exit {done.returncode}): {why(log)}")
    return done


@dataclass
class Pair:
    """One sub-pass call's snapshot files."""

    seq: str
    sub: str
    before: Path
    after: Path

    @property
    def name(self) -> str:
        return f"{self.seq}-{self.sub}"


@dataclass
class Verdict:
    """What one pair's check found; ``tail`` is Yosys's last log lines on a failure."""

    pair: Pair
    ok: bool
    tail: str = ""


@dataclass
class Run:
    """The Odin III side of one design: its pairs and its rule counters."""

    pairs: list[Pair] = field(default_factory=list)
    counters: dict[str, int] = field(default_factory=dict)
    log: str = ""


def phase2_commands(path: Path = PHASE2) -> list[str]:
    """The commands of a script file: one per line, ``#`` comments and blank lines dropped."""
    found: list[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            found.append(line)
    return found


def with_stats(commands: Sequence[str]) -> list[str]:
    """Each opt command with ``--opt-stats`` appended: the counters of the run."""
    return [f"{c} --opt-stats" if OPT_WORD.match(c) else c for c in commands]


def read_commands(files: Sequence[str], top: str | None, nolatches: bool,
                  techlib: str | None = None) -> list[str]:
    """The front of every Odin III run: ``[read_techlib L;] read_verilog [--nolatches] FILE...;
    hierarchy``."""
    flag = " --nolatches" if nolatches else ""
    return [*([f"read_techlib {quote(techlib)}"] if techlib else []),
            f"read_verilog{flag} {' '.join(quote(f) for f in files)}",
            f"hierarchy --top {top}" if top else "hierarchy -auto"]


def quote(word: str) -> str:
    """A word of an Odin III script: bare unless it holds a blank, ';', '#' or a quote."""
    if re.fullmatch(r"[^\s;#\"]+", word):
        return word
    if '"' in word:
        raise ToolError(f"cannot quote {word!r} in an Odin III script")
    return f'"{word}"'


def has_processes(text: str) -> bool:
    """True when an RTLIL text holds a ``process`` block."""
    return PROCESS_LINE.search(text) is not None


def module_names(text: str) -> list[str]:
    return MODULE_LINE.findall(text)


def top_module(text: str, wanted: str | None = None) -> str:
    """The top of an RTLIL text: the one after ``attribute \\top 1``, else ``wanted``, else the
    sole module; ``ToolError`` when none is clear."""
    names = module_names(text)
    marked: list[str] = re.findall(r"^attribute \\top 1\s*\nmodule (\\\S+)", text, re.M)
    if marked:
        return marked[0]
    named = "\\" + (wanted or "").lstrip("\\")
    if wanted and named in names:
        return named
    if len(names) == 1:
        return names[0]
    raise ToolError("no top module: several modules and no `attribute \\top 1`")


def rename_modules(text: str, side: str, top: str) -> str:
    """The text with its top module renamed ``\\<side>``, the other modules ``\\<side>.<name>``, and
    the instances of them following, so two files can share one Yosys design."""
    names = module_names(text)
    new = {n: (f"\\{side}" if n == top else f"\\{side}.{n[1:]}") for n in names}
    out = TOP_ATTR.sub("", text)
    out = MODULE_LINE.sub(lambda m: f"module {new[m.group(1)]}", out)

    def cell(match: re.Match[str]) -> str:
        kind = match.group(2)
        return f"{match.group(1)}cell {new.get(kind, kind)} "

    return re.sub(r"^(\s*)cell (\\\S+) ", cell, out, flags=re.M)


# Two families of lowering (review C1). ``async2sync`` puts every flip-flop on one implicit clock:
# it is exact, and cheap, only when each module has one clock (a primary input, one polarity) on
# both sides with the same use (``clock_sig.same_clocks``); it is blind to a changed clock net or
# edge otherwise. ``clk2fflogic`` makes each clock edge an explicit sampled clock, so the proof
# sees the clock net and its polarity, at a price: its shadow registers match nothing across the
# sides, so the induction is weak for big designs. ``opt_merge; opt_clean`` folds its
# per-flip-flop copies of the past clock (a sequential design took > 120 s without, 1 s with).
ONE_CLOCK = (("async2sync",), ("async2sync", "techmap"), ("memory", "async2sync", "techmap"))
CLK = ("clk2fflogic", "opt_merge", "opt_clean")
LOWERINGS = (CLK, (*CLK, "techmap"), ("memory", *CLK, "techmap"))


def refinement_script(gold: str, gate: str, procs: bool = False,
                      lowering: Sequence[str] = LOWERINGS[1]) -> str:
    """Spec 20.2's Yosys script for two RTLIL files that ``rename_modules`` already made
    ``gold``/``gate``; ``procs`` adds ``proc``, ``lowering`` the commands that run on both sides
    before the miter. Each side is flattened first: ``equiv_make`` matches only the two tops, so a
    submodule's flip-flops would otherwise keep independent x initial values (review I4)."""
    steps = [f"read_rtlil {gold}", f"read_rtlil {gate}", *(["proc"] if procs else []), "flatten",
             *lowering]
    steps += ["equiv_make gold gate eq", "hierarchy -top eq", "flatten",
              "equiv_simple -undef -seq 4", "equiv_induct -undef", "equiv_status -assert"]
    return "; ".join(steps)


class Checker:
    """Runs the refinement check of RTLIL files through Yosys, every child under memcap."""

    def __init__(self, yosys: str, work: Path, cap: int, lower: bool = True,
                 techmap: bool = True, top: str | None = None) -> None:
        self.yosys = exe(yosys)
        self.work = work
        self.cap = cap
        self.lower = lower
        self.techmap = techmap
        self.top = top

    def child(self, argv: Sequence[str]) -> subprocess.CompletedProcess[str]:
        return capped("yosys", argv, self.cap, cwd=self.work, timeout=REFINE_TIMEOUT_SECONDS)

    def refines(self, gold: Path, gate: Path, tag: str) -> tuple[bool, str]:
        """(True, '') when ``gate`` refines ``gold``; else (False, Yosys's last lines) after an
        exit that is Yosys's own verdict (an unproven pair, an ``ERROR``). A Yosys crash, signal,
        memory-cap kill or timeout is a ``CrashError``, never "not a refinement" (review I3)."""
        gold_text = gold.read_text(encoding="utf-8")
        gate_text = gate.read_text(encoding="utf-8")
        gold_out = self.work / f"{tag}.gold.il"
        gate_out = self.work / f"{tag}.gate.il"
        gold_out.write_text(rename_modules(gold_text, "gold", top_module(gold_text, self.top)),
                            encoding="utf-8")
        gate_out.write_text(rename_modules(gate_text, "gate", top_module(gate_text, self.top)),
                            encoding="utf-8")
        procs = has_processes(gold_text) or has_processes(gate_text)
        last = ""
        crashed: CrashError | None = None
        for lowering in self.flows(clock_sig.same_clocks(gold_text, gate_text)):
            script = refinement_script(str(gold_out), str(gate_out), procs, lowering)
            try:
                done = self.child([self.yosys, "-p", script])
            except CrashError as exc:
                if exc.timed_out:  # a timeout stops the pair
                    raise
                crashed = crashed or exc  # a lowering that aborts proves nothing: try the next
                continue
            if done.returncode == 0:
                return True, ""
            last = tail((done.stdout or "") + (done.stderr or ""))
        if crashed is not None:  # no proof, and a lowering died: not a verdict
            raise crashed
        return False, last

    def flows(self, one_clock: bool = False) -> list[Sequence[str]]:
        """The lowerings tried in order; a pass in any one is a proof (each lowers both sides
        alike). ``one_clock``: both sides have the same single clock, so ``async2sync`` is exact;
        else the clock-aware ``clk2fflogic`` family. ``techmap`` makes more cells provable and loses
        a division by zero and a ``$pmux``; ``memory`` maps the memories."""
        family = ONE_CLOCK if one_clock else LOWERINGS
        if not self.lower:
            return [()]
        return [family[0]] if not self.techmap else list(family)


def collect_pairs(snap: Path) -> list[Pair]:
    """The before/after pairs of a snapshot directory, in sequence order."""
    pairs: list[Pair] = []
    for before in sorted(snap.iterdir()):
        found = PAIR_NAME.match(before.name)
        if found is None:
            continue
        after = before.with_name(before.name.replace("-before.il", "-after.il"))
        if after.is_file():
            pairs.append(Pair(found.group(1), found.group(2), before, after))
    return pairs


def parse_counters(log: str) -> dict[str, int]:
    """The ``opt: rule NAME N`` lines of an Odin III log, summed by name."""
    counters: dict[str, int] = {}
    for name, count in STAT_LINE.findall(log):
        counters[name] = counters.get(name, 0) + int(count)
    return counters


def counter_text(counters: dict[str, int]) -> str:
    return ", ".join(f"{n} {c}" for n, c in sorted(counters.items())) or "none"


def run_odin(odin3: str, args: Sequence[str], script: str, work: Path, cap: int
             ) -> subprocess.CompletedProcess[str]:
    return capped("odin3", [exe(odin3), *args, "-p", script], cap, cwd=work)


def snapshot_run(odin3: str, files: Sequence[str], top: str | None, script: Sequence[str],
                 work: Path, cap: int, techlib: str | None = None) -> Run:
    """Odin III with ``--opt-snapshot``: the pairs it wrote and its counters. ``ToolError`` when
    it fails (``CrashError`` when it crashed or timed out) or wrote an empty snapshot file."""
    snap = work / "snap"
    snap.mkdir(exist_ok=True)
    commands = [*read_commands(files, top, True, techlib), *with_stats(script)]
    argv = [*(["--top", top] if top else []), "--opt-snapshot", str(snap)]
    done = run_odin(odin3, argv, "; ".join(commands), work, cap)
    log = (done.stdout or "") + (done.stderr or "")
    if done.returncode != 0:
        raise ToolError(f"odin3 exit {done.returncode}: {why(log)}")
    pairs = collect_pairs(snap)
    for pair in pairs:
        for path in (pair.before, pair.after):
            if not path.read_text(encoding="utf-8").strip():
                raise ToolError(f"empty snapshot file {path.name}: --opt-snapshot is broken")
    return Run(pairs, parse_counters(log), log)


def check_all(run: Run, checker: Checker) -> list[Verdict]:
    verdicts: list[Verdict] = []
    for pair in run.pairs:
        ok, reason = checker.refines(pair.before, pair.after, pair.name)
        verdicts.append(Verdict(pair, ok, reason))
    return verdicts


def report(verdicts: Sequence[Verdict], counters: dict[str, int]) -> list[str]:
    """The output lines: one per pair, the failures with the counters, then the summary."""
    lines: list[str] = []
    for v in verdicts:
        if v.ok:
            lines.append(f"{TOOL}: {v.pair.name}: ok")
        else:
            lines.append(f"{TOOL}: {v.pair.name}: not a refinement (rules: "
                         f"{counter_text(counters)}): {v.tail}")
    bad = sum(1 for v in verdicts if not v.ok)
    lines.append(f"{TOOL}: {len(verdicts)} calls changed something: "
                 f"{len(verdicts) - bad} refine, {bad} do not")
    if not verdicts:
        lines.append(f"{TOOL}: NOTE: 0 calls changed anything: nothing was checked")
    return lines


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--top", help="the top module (odin3 --top and hierarchy --top)")
    ap.add_argument("--script", type=Path, default=PHASE2, help="the script (default Phase 2's)")
    ap.add_argument("--techlib", help="read this tech library (.o3lib) first: the hard blocks")
    ap.add_argument("--no-lower", action="store_true",
                    help="no clk2fflogic and techmap (the clock is NOT checked)")
    ap.add_argument("--no-techmap", action="store_true", help="clk2fflogic only")
    ap.add_argument("--mem-cap", type=parse_size, default=DEFAULT_CAP, metavar="SIZE",
                    help="per child (default 2G)")
    ap.add_argument("--work", type=Path, help="the work directory (kept)")
    ap.add_argument("--min-changed", type=int, default=0, metavar="N",
                    help="fail (exit 1) unless at least N calls changed something")
    ap.add_argument("files", nargs="+", help="the Verilog files of one design")
    return ap.parse_args(argv)


def execute(args: argparse.Namespace, work: Path) -> tuple[int, bool]:
    """(exit code, whether the work directory is worth keeping: a pair failed)."""
    files = [str(Path(f).resolve()) for f in args.files]
    techlib = str(Path(args.techlib).resolve()) if args.techlib else None
    run = snapshot_run(args.odin3, files, args.top, phase2_commands(args.script), work,
                       args.mem_cap, techlib)
    checker = Checker(args.yosys, work, args.mem_cap, not args.no_lower, not args.no_techmap,
                      args.top)
    verdicts = check_all(run, checker)
    for line in report(verdicts, run.counters):
        print(line, flush=True)
    if len(verdicts) < args.min_changed:
        print(f"{TOOL}: {len(verdicts)} calls changed something, want at least "
              f"{args.min_changed}", flush=True)
        return EXIT_FAIL, False
    bad = any(not v.ok for v in verdicts)
    return (EXIT_FAIL if bad else EXIT_OK), bad


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    made = args.work is None
    work = Path(tempfile.mkdtemp(prefix="opt-equiv-")) if made else args.work
    work.mkdir(parents=True, exist_ok=True)
    work = work.resolve()
    code, keep = EXIT_ERROR, False
    try:
        code, keep = execute(args, work)
    except CrashError as exc:
        print(f"{TOOL}: HARD: {exc}", file=sys.stderr)
        code = EXIT_CRASH
    except (OSError, ToolError, ValueError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        code = EXIT_SKIP
    if made and code != EXIT_CRASH and not keep:
        shutil.rmtree(work, ignore_errors=True)  # the directory this run made, and only it
    elif made:
        print(f"{TOOL}: work directory kept: {work}", file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
