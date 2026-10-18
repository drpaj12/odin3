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

A hard block (a black box module such as ``single_port_ram``) is cut at its ports before the
lowering (``EVERT_BLACKBOXES``): its outputs become shared inputs, the nets on its inputs become
outputs the check compares. Each instance is first declared at the widths it connects
(``hard_blocks``; the tech library's ``dynports`` declaration is 1 bit wide, which compared only
bit 0), and a hard block that both sides hold under one name with another type, other port widths
or another parameter value is "not a refinement".

``equiv_simple``/``equiv_induct`` never start from the initial state, so a passed proof counts
only when a bounded check from the initial state (``init`` values honoured, other flip-flops x)
finds no difference in the first ``BASE_STEPS`` steps on any wire both sides name (the base case
of the induction, ``Checker.judge``). The base case failing is "not a refinement" when the same
check finds an output trace in ``BMC_STEPS`` steps, else "inconclusive"; a base case that times
out is "inconclusive" (a size limit). A failed proof stays "not a refinement"; no path turns a
failed or undecided proof into "ok".

A failing pair prints ``opt-equiv: <NNNN>-<sub>: not a refinement`` with the rule counters of the
run (Odin III's ``--opt-stats`` lines, which cover the whole run). A work directory this tool
made is removed after a clean run; a failing run, and ``--work DIR``, keep it.

``--min-changed N`` fails (exit 1) a run in which fewer than N calls changed something: a rule
source is built to make a rule fire, so ``--min-changed 1`` tells a broken or empty
``--opt-snapshot`` from "nothing to improve". A run with no changing call and no ``N`` prints a
``NOTE``. An empty snapshot file is an error. The work directory this tool made (under
``$TMPDIR``) is removed after a clean run, after an error (exit 2) and after a ``--min-changed``
shortfall; a pair that does not refine, and a crash (exit 3), keep it.

Exit codes (one convention across the opt tools): 0 every pair refines; 1 a pair does not (or the
whole-script cec says the netlists differ); 2 usage, Odin III or a tool error; 3 hard: Odin III or
a Yosys proof child crashed or was killed by a signal; 4 unproven: no pair failed, but some proofs
hit the SAT size limit (a timeout, or the checker's memory-cap kill) and the BLIF cec of the whole
script gave no verdict either; a tool limit, not a crash and no verdict on Odin III (the
elab-equiv rule: a cap kill of the checker is unproven, a cap kill of Odin III stays hard; a sweep
may list a design with this exit as ``untested for now (size limit)``); 77 skipped (an ASan child
and no systemd-run).
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

import clock_sig
import hard_blocks as hb
import memcap
import toolutil

TOOL = "opt-equiv"
HERE = Path(__file__).resolve().parent
PHASE2 = HERE / "phase2.script"
EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_CRASH, EXIT_UNPROVEN, EXIT_SKIP = 0, 1, 2, 3, 4, 77
DEFAULT_CAP = 2 << 30
TIMEOUT_SECONDS = 600.0
REFINE_TIMEOUT_SECONDS = 60.0  # one equivalence proof; past it the answer is "inconclusive"
MAX_SAT_TIMEOUTS = 2  # opt-equiv stops trying SAT proofs of a design after this many timeouts
PAIR_NAME = re.compile(r"^(\d{4})-(opt_[a-z]+|wreduce)-before\.il$")
STAT_LINE = re.compile(r"opt: rule (\S+) (\d+)\s*$", re.M)
MODULE_LINE = re.compile(r"^module (\\\S+)\s*$", re.M)
TOP_ATTR = re.compile(r"^attribute \\top 1\s*\n", re.M)
PROCESS_LINE = re.compile(r"^\s*process\s", re.M)
OPT_WORD = re.compile(r"^(opt(_[a-z]+)?|wreduce)(\s|$)")


class ToolError(Exception):
    """A bad argument or an unusable tool (exit 2)."""


class RefineAbort(ToolError):
    """Yosys aborted the refinement script with an ``ERROR`` other than the failed proof."""


class CrashError(ToolError):
    """A child crashed, was killed (signal, memory cap) or timed out (exit 3, hard). ``what`` names
    the step (``odin3``, ``yosys``, ``netlist-compare``...)."""

    def __init__(self, what: str, message: str, timed_out: bool = False,
                 cap_killed: bool = False) -> None:
        super().__init__(f"{what}: {message}")
        self.what = what
        self.timed_out = timed_out
        self.cap_killed = cap_killed  # SIGKILL: the memory cap (or the OOM killer)

    @property
    def size_limit(self) -> bool:
        """A timeout or a cap kill of a proof: the design is too big for the check, not broken."""
        return self.timed_out or self.cap_killed


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
        raise CrashError(what, f"killed (exit {done.returncode}): {why(log)}",
                         cap_killed=done.returncode in (-9, 137))
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
    inconclusive: bool = False  # the SAT proof timed out (neither ok nor a counterexample)


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


# A hard block (``single_port_ram``, ``dual_port_ram``, a multiplier: a black box module) has no
# model, and ``rename_modules`` makes the two sides' copies of it two different modules, so
# ``equiv_make`` pairs the output wires of the two instances and can never prove them equal (every
# VTR design with a memory failed this way, however small the rewrite: ch_intrinsics, mkPktMerge).
# ``expose -evert`` over the cells that are not internal ($-) cells removes each instance and turns
# its input ports into extra outputs and its output ports into extra inputs, named by the
# instance, so the two sides share the black box's outputs and the check compares what drives its
# inputs. Sound: equal inputs to one deterministic block give equal outputs; an opt call that
# changed an input of the block is a counterexample. With no black box the selection is empty.
EVERT_BLACKBOXES = "expose -evert c:* t:$* %d"


def refinement_script(gold: str, gate: str, procs: bool = False,
                      lowering: Sequence[str] = LOWERINGS[1], purge: bool = False) -> str:
    """Spec 20.2's Yosys script for two RTLIL files that ``rename_modules`` already made
    ``gold``/``gate``; ``procs`` adds ``proc``, ``lowering`` the commands that run on both sides
    before the miter; ``purge`` adds ``opt_clean -purge`` after them, which drops the wires nothing
    reads (a public wire whose unused bits wreduce turns into x has no observable value, and
    ``equiv_make`` would match it by name). Purging also takes away names that help ``equiv_make``
    pair registers, so ``Checker.refines`` tries each lowering plain first, then purged. Each side
    is flattened first: ``equiv_make`` matches only the two tops, so a
    submodule's flip-flops would otherwise keep independent x initial values (review I4)."""
    steps = [f"read_rtlil {gold}", f"read_rtlil {gate}", *(["proc"] if procs else []), "flatten",
             EVERT_BLACKBOXES, *lowering]
    steps += ["opt_clean -purge"] if purge else []
    steps += ["equiv_make gold gate eq", "hierarchy -top eq", "flatten",
              "equiv_simple -undef -seq 4", "equiv_induct -undef", "equiv_status -assert"]
    return "; ".join(steps)


# Yosys 0.55 ``equiv_status -assert``: ``log_error("Found %d unproven $equiv cells in
# 'equiv_status -assert'.")``: the refinement script saying the proof failed. The second is the
# documented limit of the SAT flow (``$pow`` has no model, spec 20.2): no proof, so not a
# refinement either. Any other ``ERROR`` (parser, port, missing module) is no verdict.
# ``equiv_make`` refuses a design with memories or processes: the plain and the techmap lowering
# do so for every design with a memory (it is why the third lowering runs ``memory`` first), so that
# refusal is not a tool error and must not hide what the lowering that can judge did (a timeout).
NEEDS_MEMORY = re.compile(r"ERROR: Gold module contains memories or processes")
PROOF_FAILED = re.compile(r"ERROR: (?:Found \d+ unproven \$equiv cells in 'equiv_status -assert'"
                          r"|No SAT model available for cell )")


EQUIV_CHECK = HERE.parent / "equiv-check" / "equiv-check"
CEC_TIMEOUT_SECONDS = 900.0
CEC_PROVED, CEC_DIFFERS, CEC_UNPROVEN = "proved", "differs", "unproven"
CEC_LOWERING = ("proc", "hierarchy -auto-top", "flatten", "memory", "techmap", "opt -fast",
                "dffunmap", "opt -fast -noff", "opt_clean -purge",
                "write_blif -true + vcc -false + gnd -undef + unconn -blackbox")


def cec_lowering_script(il: Path, blif: Path) -> str:
    """One side to a flat BLIF for ``equiv-check``: the lowering of opt-diff's identity step,
    with ``proc`` first (a snapshot from before ``proc`` holds processes)."""
    steps = [*CEC_LOWERING[:-1], f"{CEC_LOWERING[-1]} {blif}"]
    return "; ".join([f"read_rtlil {il}", *steps])


X_NET = re.compile(rb"(?<![^\s=])unconn(?![^\s=])")  # the whole token, never a longer name
X_NET_DEFINITION = b".names unconn\n"


def uses_x(blif: Path) -> bool:
    """True when a BLIF written by Yosys reads the net ``unconn`` (an x: Yosys's ``write_blif
    -undef unconn``), apart from its own empty cover. equiv-check ties it to 0 on both sides, so
    a "not equivalent" of a design with x is a resolution of the x, not a refutation (spec 20.2:
    an x in the gold is a don't care; stereovision0 and 2 are 'equivalent outside the x-cones')."""
    try:
        data = blif.read_bytes()
    except OSError:
        return False
    return len(X_NET.findall(data)) > data.count(X_NET_DEFINITION)


def cec_blifs(a: Path, b: Path, cap: int, work: Path) -> tuple[str, str]:
    """(verdict, text) of ``equiv-check`` on two BLIFs: proved (exit 0), differs (exit 1), else
    unproven (a tool limit: hard-block models, several clocks, a timeout; the text says which). A
    timeout or a crash of the checker is "unproven" too, never a pass."""
    try:
        done = capped("equiv-check", [exe(str(EQUIV_CHECK)), str(a), str(b)], cap, cwd=work,
                      timeout=CEC_TIMEOUT_SECONDS)
    except CrashError as exc:
        return CEC_UNPROVEN, f"BLIF cec: {exc}"
    text = why((done.stdout or "") + (done.stderr or ""))
    if done.returncode == 0:
        return CEC_PROVED, ""
    if done.returncode == 1 and (uses_x(a) or uses_x(b)):
        return CEC_UNPROVEN, ("BLIF cec: not equivalent with the x nets (unconn) tied to 0, which "
                              "cannot express that an x in the gold is a don't care (the pair "
                              f"checks do): no verdict either way: {text}")
    if done.returncode == 1:
        return CEC_DIFFERS, f"BLIF cec: not equivalent: {text}"
    return CEC_UNPROVEN, f"BLIF cec: no verdict (exit {done.returncode}): {text}"


def cec_ils(yosys: str, gold: Path, gate: Path, cap: int, work: Path, tag: str
            ) -> tuple[str, str]:
    """``cec_blifs`` of the lowering of two RTLIL files."""
    blifs: list[Path] = []
    for side, il in (("gold", gold), ("gate", gate)):
        blif = work / f"{tag}.{side}.blif"
        try:
            done = capped("yosys (cec lowering)", [exe(yosys), "-p", cec_lowering_script(il, blif)],
                          cap, cwd=work)
        except CrashError as exc:
            return CEC_UNPROVEN, f"BLIF cec: {exc}"
        if done.returncode != 0:
            return CEC_UNPROVEN, (f"BLIF cec: the lowering of the {side} side failed: "
                                  f"{why((done.stdout or '') + (done.stderr or ''))}")
        blifs.append(blif)
    return cec_blifs(blifs[0], blifs[1], cap, work)


# ---- The base case: a bounded check from the initial state (3B soundness review H2) ----------
#
# ``equiv_simple``/``equiv_induct`` prove that gold and gate never diverge once their named wires
# agree; neither starts from the initial state (equiv_induct's base case only asks that some
# consistent trace exists), so ``reg q = 0`` against ``reg q = 1`` passed. A passed proof stands
# only when a bounded model check from the initial state, over every public wire both sides name
# (the wires ``equiv_make`` pairs, made outputs by ``expose -shared``), finds no difference in the
# first BASE_STEPS steps. ``equiv_simple -seq 4`` proves a cell over at most 5 time frames from a
# free state (equiv_simple.cc: ``step`` from 4 down to 0), the other ``$equiv`` cells cut points,
# and ``equiv_induct`` steps from any state where they all hold, so a step t >= 4 is covered once
# all named wires agree in steps 0..3: BASE_STEPS = 4.
#
# The model: both sides as the proof had them (flattened, hard blocks cut), ``memory``, then the
# proof's clock model (``async2sync``, one step per cycle, when ``clock_sig.same_clocks``; else
# ``clk2fflogic``), ``opt_clean`` (``-purge`` when the proof passed purged: it paired only the
# wires left), ``miter -equiv -ignore_gold_x`` (an x in the gold is a don't care) and ``sat -seq N
# -set-init-undef -enable_undef -set-def-inputs``: every ``init`` attribute is honoured and a
# flip-flop without one starts at x, its RTLIL meaning (lower-equiv's ``-set-init-zero`` is VTR's
# power-up value for a BLIF gate; here both sides are RTLIL and ``opt_dff`` may replace a
# flip-flop that never changes and has no init by an x: a refinement that a 0 would call a
# difference). When the base case fails, the output check
# (``clk2fflogic``, outputs only, BMC_STEPS = 20 half-cycles: 10 edges of each clock) decides "not a
# refinement" on a trace, else "inconclusive". Each check stops after ``REFINE_TIMEOUT_SECONDS``;
# a timeout is a size limit, a Yosys error no answer: neither is a proof. No ``sat -verify``: its
# ``log_error`` exits before Yosys flushes stdout (the same rule as lower-equiv, 9ac875a4).
BASE_STEPS = 4
BMC_STEPS = 20
TRACE, NO_TRACE, NO_ANSWER, SIZE_LIMIT = "trace", "none", "unknown", "size limit"
SAT_FAIL = re.compile(r"SAT proof finished - model found: FAIL!")
SAT_PASS = re.compile(r"SAT proof finished - no model found: SUCCESS!")


def bounded_script(gold: str, gate: str, procs: bool, steps: int,
                   model: tuple[bool, bool, bool] = (False, False, False)) -> str:
    """The Yosys script of the bounded check of two files ``rename_modules`` made gold/gate.
    ``model`` = (shared: every public wire both sides name is compared too, the base case;
    one_clock: ``async2sync`` steps instead of ``clk2fflogic``'s; purge: ``opt_clean -purge``)."""
    shared, one_clock, purge = model
    script = [f"read_rtlil {gold}", f"read_rtlil {gate}", *(["proc"] if procs else []),
              "flatten", EVERT_BLACKBOXES, "memory",
              "async2sync" if one_clock else "clk2fflogic",
              "opt_clean -purge" if purge else "opt_clean",
              *(["expose -shared gold gate"] if shared else []),
              "miter -equiv -flatten -make_assert -ignore_gold_x gold gate miter",
              "hierarchy -top miter",
              f"sat -seq {steps} -set-init-undef -enable_undef -set-def-inputs "
              "-prove-asserts -show-inputs miter"]
    return "; ".join(script)


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
        self.passed: tuple[Sequence[str], bool] | None = None  # the last proof: lowering, purge

    def child(self, argv: Sequence[str]) -> subprocess.CompletedProcess[str]:
        return capped("yosys-refine", argv, self.cap, cwd=self.work,
                      timeout=REFINE_TIMEOUT_SECONDS)

    def refines(self, gold: Path, gate: Path, tag: str) -> tuple[bool, str]:
        """(True, '') when ``gate`` refines ``gold``; else (False, Yosys's last lines) after an
        exit whose output is the failed proof itself (``PROOF_FAILED``). A Yosys crash, signal,
        memory-cap kill or timeout is a ``CrashError`` (exit 3); any other non-zero exit (a parser
        ``ERROR``, a port mismatch, a missing module) is a ``ToolError`` (exit 2): neither is "not a
        refinement" and no expect or skip list may absorb it (review I3)."""
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
        aborted: RefineAbort | None = None
        refused: RefineAbort | None = None  # a lowering that cannot take memories (NEEDS_MEMORY)
        attempts = [(lowering, purge)
                    for purge in (False, True)
                    for lowering in self.flows(clock_sig.same_clocks(gold_text, gate_text))]
        for lowering, purge in attempts:
            script = refinement_script(str(gold_out), str(gate_out), procs, lowering, purge)
            try:
                done = self.child([self.yosys, "-p", script])
            except CrashError as exc:
                if exc.size_limit:  # a timeout or a cap kill stops the pair
                    if aborted is not None and exc.timed_out:  # (a lowering already said no)
                        raise aborted from exc
                    raise
                crashed = crashed or exc  # a lowering that aborts proves nothing: try the next
                continue
            if done.returncode == 0:
                self.passed = (lowering, purge)
                return True, ""
            output = (done.stdout or "") + (done.stderr or "")
            if not PROOF_FAILED.search(output):  # an ERROR that is not the failed proof: this
                # lowering cannot judge the pair (equiv_make refuses memories before `memory`)
                abort = RefineAbort(f"yosys-refine exit {done.returncode}: {why(output)}")
                if NEEDS_MEMORY.search(output):
                    refused = refused or abort
                else:
                    aborted = aborted or abort
                continue
            last = tail(output)
        if crashed is not None:  # no proof, and a lowering died: not a verdict
            raise crashed
        pending = aborted or refused
        if pending is not None and not last:  # no lowering produced a failed proof either
            raise pending
        return False, last

    def judge(self, gold: Path, gate: Path, tag: str) -> tuple[bool, str, bool]:
        """(ok, text, inconclusive) for one pair: the hard blocks at full width (a shared block
        that differs is not a refinement), the proof (``refines``; a failed proof is not a
        refinement), then the base case from the initial state. ``ok`` only when the proof and
        the base case both pass. A proof timeout propagates as ``refines``'s ``CrashError``, a
        base-case timeout raises one too (both a size limit for ``check_all``)."""
        texts = [gold.read_text(encoding="utf-8"), gate.read_text(encoding="utf-8")]
        tops = (top_module(texts[0], self.top), top_module(texts[1], self.top))
        gold, gate, mismatch = self.prepare_boxes((gold, gate), tops, tag)
        if mismatch:
            return False, mismatch, False
        self.passed = None
        ok, last = self.refines(gold, gate, tag)
        if not ok:
            return False, last, False
        purge = self.passed[1] if self.passed else False
        found, text = self.bounded((gold, gate), tag, BASE_STEPS, (True, purge))
        if found == NO_TRACE:
            return True, "", False
        if found == SIZE_LIMIT:
            raise CrashError("yosys-bmc", f"base case from the initial state: {text}", True)
        trace, detail = self.bounded((gold, gate), tag, BMC_STEPS, (False, False))
        if trace == TRACE:
            return False, f"the initial states differ: bounded check: {detail}", False
        outputs = (f"no output trace in {BMC_STEPS} steps" if trace == NO_TRACE else
                   f"the output check gave no answer ({detail[-160:]})")
        return False, (f"the proof passed but its base case from the initial state "
                       f"{'failed' if found == TRACE else 'gave no answer'} on the named wires "
                       f"({text[-200:]}) and {outputs}"), True

    def bounded(self, sides: tuple[Path, Path], tag: str, steps: int,
                shared_purge: tuple[bool, bool]) -> tuple[str, str]:
        """(TRACE | NO_TRACE | NO_ANSWER | SIZE_LIMIT, Yosys's last lines) of the bounded check
        of ``sides`` (gold, gate). ``shared_purge`` = (the base case: every named wire, in the
        proof's clock model; ``opt_clean -purge``). Only the base case uses async2sync."""
        shared, purge = shared_purge
        texts = [path.read_text(encoding="utf-8") for path in sides]
        names = []
        for text, side in zip(texts, ("gold", "gate"), strict=True):
            path = self.work / f"{tag}.bmc.{side}.il"
            path.write_text(rename_modules(text, side, top_module(text, self.top)),
                            encoding="utf-8")
            names.append(str(path))
        procs = any(has_processes(text) for text in texts)
        one_clock = shared and self.lower and clock_sig.same_clocks(texts[0], texts[1])
        script = bounded_script(names[0], names[1], procs, steps, (shared, one_clock, purge))
        try:
            done = capped("yosys-bmc", [self.yosys, "-p", script], self.cap, cwd=self.work,
                          timeout=REFINE_TIMEOUT_SECONDS)
        except CrashError as exc:
            return (SIZE_LIMIT if exc.size_limit else NO_ANSWER), str(exc)
        log = (done.stdout or "") + (done.stderr or "")
        if done.returncode == 0 and SAT_FAIL.search(log):
            return TRACE, why(log)
        if done.returncode == 0 and SAT_PASS.search(log):
            return NO_TRACE, ""
        return NO_ANSWER, f"bounded check exit {done.returncode}: {why(log)}"

    def prepare_boxes(self, sides: tuple[Path, Path], tops: tuple[str, str], tag: str
                      ) -> tuple[Path, Path, str]:
        """(gold, gate, difference) with every hard block declared at its instance's widths
        (``hard_blocks``). The paths are unchanged when neither side has a user cell;
        ``difference`` is non-empty when a shared hard block differs in type, widths or a
        parameter. A Yosys error or a block that cannot be cut is a ``ToolError``."""
        texts = [path.read_text(encoding="utf-8") for path in sides]
        if not any(hb.has_user_cells(text) for text in texts):
            return sides[0], sides[1], ""
        modules = {**hb.blackbox_modules(texts[1]), **hb.blackbox_modules(texts[0])}
        probes = [self.flat_probe(path, f"{tag}.box.{side}")
                  for path, side in zip(sides, ("gold", "gate"), strict=True)]
        try:
            uses = [hb.box_uses(design, top, set(modules))
                    for (_, design), top in zip(probes, tops, strict=True)]
            names, decls = hb.declarations(modules, uses)
            out = []
            for (flat, _), side, top, side_uses in zip(probes, ("gold", "gate"), tops, uses,
                                                       strict=True):
                text = hb.retype_boxes(flat.read_text(encoding="utf-8"), top, side_uses, names)
                path = self.work / f"{tag}.box.{side}.il"
                path.write_text(hb.mark_top(text, top) + decls, encoding="utf-8")
                out.append(path)
        except hb.BoxError as exc:
            raise ToolError(f"hard blocks: {exc}") from exc
        return out[0], out[1], hb.box_mismatch(uses[0], uses[1])

    def flat_probe(self, il: Path, tag: str) -> tuple[Path, dict[str, object]]:
        """(``<tag>.flat.il``, its ``write_json`` design): ``il`` flattened (after ``proc`` when
        it holds processes), as the refinement script does first anyway."""
        flat, dump = self.work / f"{tag}.flat.il", self.work / f"{tag}.flat.json"
        procs = "proc; " if has_processes(il.read_text(encoding="utf-8")) else ""
        script = f"read_rtlil {il}; {procs}flatten; write_rtlil {flat}; write_json {dump}"
        done = capped("yosys-flatten", [self.yosys, "-p", script], self.cap, cwd=self.work)
        if done.returncode != 0:
            raise ToolError(f"yosys-flatten exit {done.returncode}: "
                            f"{why((done.stdout or '') + (done.stderr or ''))}")
        design: dict[str, object] = json.loads(dump.read_text(encoding="utf-8"))
        return flat, design

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
    """A pair whose SAT proof or base case times out is "inconclusive" (the run goes on), as is
    one whose base case ``Checker.judge`` cannot settle; every other crash stays a
    ``CrashError``."""
    verdicts: list[Verdict] = []
    timeouts = 0
    for pair in run.pairs:
        if timeouts >= MAX_SAT_TIMEOUTS:  # the rest of this design is as big: not tried
            verdicts.append(Verdict(pair, False, f"not tried after {timeouts} SAT size-limit hits",
                                    True))
            continue
        try:
            ok, reason, unsure = checker.judge(pair.before, pair.after, pair.name)
        except CrashError as exc:
            if not exc.size_limit:
                raise
            timeouts += 1
            verdicts.append(Verdict(pair, False, str(exc), True))
            continue
        verdicts.append(Verdict(pair, ok, reason, unsure))
    return verdicts


def settle_inconclusive(run: Run, verdicts: Sequence[Verdict], args: argparse.Namespace,
                        work: Path) -> tuple[str, str]:
    """When no pair failed but some timed out: the whole script, Odin III's input against its
    final result, through the BLIF cec. (verdict, text); ``CEC_PROVED`` clears the timeouts."""
    if not run.pairs:
        return CEC_UNPROVEN, "no snapshot"
    return cec_ils(args.yosys, run.pairs[0].before, run.pairs[-1].after, args.mem_cap, work,
                   "e2e")


def report(verdicts: Sequence[Verdict], counters: dict[str, int]) -> list[str]:
    """The output lines: one per pair, the failures with the counters, then the summary."""
    lines: list[str] = []
    for v in verdicts:
        if v.inconclusive:
            lines.append(f"{TOOL}: {v.pair.name}: inconclusive ({v.tail})")
        elif v.ok:
            lines.append(f"{TOOL}: {v.pair.name}: ok")
        else:
            lines.append(f"{TOOL}: {v.pair.name}: not a refinement (rules: "
                         f"{counter_text(counters)}): {v.tail}")
    slow = sum(1 for v in verdicts if v.inconclusive)
    bad = sum(1 for v in verdicts if not v.ok and not v.inconclusive)
    lines.append(f"{TOOL}: {len(verdicts)} calls changed something: "
                 f"{len(verdicts) - bad - slow} refine, {bad} do not"
                 + (f", {slow} inconclusive (SAT size limit or initial state unproven)"
                    if slow else ""))
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
    ap.add_argument("--refine-timeout", type=float, default=REFINE_TIMEOUT_SECONDS, metavar="S",
                    help="seconds for one SAT proof (default 60); a pair that times out falls "
                         "back to a BLIF cec of the whole script")
    ap.add_argument("--no-cec", action="store_true",
                    help="no BLIF cec fallback: a SAT timeout is unproven")
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
    bad = any(not v.ok and not v.inconclusive for v in verdicts)
    slow = sum(1 for v in verdicts if v.inconclusive)
    if slow and not bad and not args.no_cec:
        verdict, text = settle_inconclusive(run, verdicts, args, work)
        if verdict == CEC_PROVED:
            print(f"{TOOL}: {slow} calls hit the SAT size limit (timeout or cap kill); the whole "
                  "script is proved by the BLIF cec (input against final result)", flush=True)
            slow = 0
        else:
            print(f"{TOOL}: unproven: {slow} calls hit the SAT size limit (timeout or cap kill) "
                  f"and the whole script is "
                  f"{'NOT equivalent' if verdict == CEC_DIFFERS else 'not decided'} by the BLIF "
                  f"cec: {text}", flush=True)
            return (EXIT_FAIL if verdict == CEC_DIFFERS else EXIT_UNPROVEN), True
    elif slow and not bad:
        print(f"{TOOL}: unproven: {slow} calls hit the SAT size limit (timeout or cap kill)",
              flush=True)
        return EXIT_UNPROVEN, True
    if len(verdicts) < args.min_changed:
        print(f"{TOOL}: {len(verdicts)} calls changed something, want at least "
              f"{args.min_changed}", flush=True)
        return EXIT_FAIL, False
    return (EXIT_FAIL if bad else EXIT_OK), bad


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    globals()["REFINE_TIMEOUT_SECONDS"] = args.refine_timeout
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
