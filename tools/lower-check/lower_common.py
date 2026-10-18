"""Shared pieces of lower-diff and lower-sim (3B spec 14.2, 14.3; plan Task 2): the exit codes of
the opt tools' convention, the work directory that is never removed, the cell-type histograms of
an RTLIL text, and Odin III's script front.

The convention (2F's tools): exit 0 every verdict holds, 1 a difference, 2 a tool error or usage,
3 hard (a child crashed, was killed by a signal or its memory cap, or timed out; no expect list
absorbs it), 77 skipped (an ASan child and no systemd-run). Every child runs through
``memcap.run``. A work directory is made under ``$ODIN3_LOWER_WORK`` (else ``$TMPDIR``) and is
left in place: these tools never delete.
"""

from __future__ import annotations

import os
import re
import tempfile
from collections import Counter
from collections.abc import Sequence
from pathlib import Path

import opt_equiv

EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_CRASH, EXIT_SKIP = 0, 1, 2, 3, 77
WORK_ENV = "ODIN3_LOWER_WORK"
DEFAULT_CAP = 3 << 30  # one Yosys or Odin III child (plan Task 2: prlimit --as=3 GiB)
TIMEOUT_SECONDS = opt_equiv.TIMEOUT_SECONDS
YOSYS_VERSION = "0.55"

ToolError = opt_equiv.ToolError
CrashError = opt_equiv.CrashError
capped = opt_equiv.capped
exe = opt_equiv.exe
why = opt_equiv.why
parse_size = opt_equiv.parse_size
quote = opt_equiv.quote

MODULE_LINE = re.compile(r"^module\s+(\S+)\s*$")
CELL_LINE = re.compile(r"^\s*cell\s+(\S+)\s+(\S+)\s*$")
CELL_TYPE_ANYWHERE = re.compile(r"^\s*cell\s+(\S+)\s+\S+\s*$", re.M)
IGNORED_TYPES = ("$_CONST",)  # constants are cells here and `connect`s in Yosys


def make_work(requested: Path | None, prefix: str) -> Path:
    """The work directory: ``requested`` (created if absent), else a fresh one under
    ``$ODIN3_LOWER_WORK`` or ``$TMPDIR``. Never removed by these tools."""
    if requested is not None:
        requested.mkdir(parents=True, exist_ok=True)
        return requested.resolve()
    return Path(tempfile.mkdtemp(prefix=prefix, dir=os.environ.get(WORK_ENV) or None)).resolve()


STORAGE_SUBCKT = {"$_DFF_P_": ("re", "C"), "$_DFF_N_": ("fe", "C"), "$_DLATCH_P_": ("ah", "E"),
                  "$_DLATCH_N_": ("al", "E")}


def storage_to_latch(blif: str) -> str:
    """Rewrites Yosys's ``.subckt $_DFF_P_ C=c D=d Q=q`` (``write_blif -icells``) as the ``.latch d
    q re c 3`` Odin III writes (``fe`` for ``$_DFF_N_``, ``ah``/``al`` with the enable for the
    latches); the init is 3, as the cells carry no ``INIT`` in Yosys's text. Other lines are kept.
    """
    out: list[str] = []
    for line in blif.splitlines():
        words = line.split()
        if len(words) >= 2 and words[0] == ".subckt" and words[1] in STORAGE_SUBCKT:
            kind, clock = STORAGE_SUBCKT[words[1]]
            pins = dict(word.split("=", 1) for word in words[2:] if "=" in word)
            if {clock, "D", "Q"} <= set(pins):
                out.append(f".latch {pins['D']} {pins['Q']} {kind} {pins[clock]} 3")
                continue
        out.append(line)
    return "\n".join(out) + ("\n" if blif.endswith("\n") else "")


LATCH_TYPES = {"re": "$_DFF_P_", "fe": "$_DFF_N_", "ah": "$_DLATCH_P_", "al": "$_DLATCH_N_"}


def blif_histogram(blif: str) -> dict[str, Counter[str]]:
    """Per ``.model``, the count of every ``.subckt`` type and ``.latch`` kind (a ``re`` latch is
    ``$_DFF_P_``, ``fe`` ``$_DFF_N_``, ``ah``/``al`` the level latches) of a BLIF text. Used where
    Odin III cannot write RTLIL (the storage bit cells have no RTLIL form, 1F); ``.names`` covers
    are counted as ``.names``."""
    found: dict[str, Counter[str]] = {}
    current: Counter[str] | None = None
    for line in blif.splitlines():
        words = line.split()
        if not words:
            continue
        if words[0] == ".model" and len(words) > 1:
            current = found.setdefault(words[1], Counter())
        elif current is not None and words[0] == ".subckt" and len(words) > 1:
            current[words[1]] += 1
        elif current is not None and words[0] == ".latch":
            current[LATCH_TYPES.get(words[3] if len(words) > 3 else "", "$_FF_")] += 1
        elif current is not None and words[0] == ".names":
            current[".names"] += 1
    return found


def histogram(rtlil: str) -> dict[str, Counter[str]]:
    """Per module, the count of every cell type of an RTLIL text (constants left out, a module's
    leading backslash dropped)."""
    found: dict[str, Counter[str]] = {}
    current: Counter[str] | None = None
    for line in rtlil.splitlines():
        module = MODULE_LINE.match(line)
        if module:
            current = found.setdefault(module.group(1).lstrip("\\"), Counter())
            continue
        cell = CELL_LINE.match(line)
        if cell and current is not None and not cell.group(1).startswith(IGNORED_TYPES):
            current[cell.group(1)] += 1
    return found


def cell_types(rtlil: str) -> set[str]:
    """Every cell type an RTLIL text instantiates."""
    return set(CELL_TYPE_ANYWHERE.findall(rtlil))


def histogram_text(counts: Counter[str]) -> str:
    return ", ".join(f"{name} {n}" for name, n in sorted(counts.items())) or "none"


def histogram_diff(odin: dict[str, Counter[str]], yosys: dict[str, Counter[str]]) -> str:
    """Empty when the per-module histograms agree, else one line per module that differs."""
    lines: list[str] = []
    for name in sorted(set(odin) | set(yosys)):
        lhs, rhs = odin.get(name, Counter()), yosys.get(name, Counter())
        if lhs != rhs:
            lines.append(f"module {name}: odin3 {histogram_text(lhs)}; yosys {histogram_text(rhs)}")
    return "; ".join(lines)


def read_front(files: Sequence[str], top: str | None, techlib: str | None,
               nolatches: bool = True) -> list[str]:
    """Odin III's front of every run: ``[read_techlib L;] read_verilog --nolatches FILE...;
    hierarchy`` (a ``.blif`` file is ``read_blif`` instead, and needs no hierarchy). The latch
    fixtures pass ``nolatches=False``: the flag turns an inferred latch into a feedback mux."""
    if len(files) == 1 and files[0].endswith(".blif"):
        return [f"read_blif {quote(files[0])}"]
    return opt_equiv.read_commands(files, top, nolatches, techlib)


def odin_args(top: str | None, extra: Sequence[str] = ()) -> list[str]:
    return [*(["--top", top] if top else []), *extra]


def _blif_blocks(text: str) -> list[list[str]]:
    """The lines of a BLIF text grouped into directive blocks (a ``.names`` with its cover rows)."""
    blocks: list[list[str]] = []
    for line in text.splitlines():
        if line.startswith(".") or not blocks:
            blocks.append([line])
        else:
            blocks[-1].append(line)
    return blocks


def _blif_uses(blocks: list[list[str]]) -> set[str]:
    """Every net a block reads: ``.names`` inputs, ``.subckt`` and ``.gate`` pins, latch inputs and
    the model outputs."""
    used: set[str] = set()
    for block in blocks:
        words = block[0].split()
        if not words:
            continue
        if words[0] == ".names":
            used.update(words[1:-1])
        elif words[0] in (".subckt", ".gate"):
            used.update(w.split("=", 1)[1] for w in words[2:] if "=" in w)
        elif words[0] == ".latch":
            used.add(words[1])
        elif words[0] == ".outputs":
            used.update(words[1:])
    return used


def strip_unused_constants(blif: str) -> str:
    """The BLIF without its unused constant generators: Yosys's ``write_blif`` always writes
    ``.names`` drivers for the false, true and undefined nets, used or not, which Odin III does
    not; ``netlist-compare`` would call that a difference. A zero-input ``.names`` whose net nothing
    reads (and that is no output) is dropped."""
    blocks = _blif_blocks(blif.replace("\\\n", " "))  # a "\" line continuation is one line
    used = _blif_uses(blocks)
    kept: list[str] = []
    for block in blocks:
        words = block[0].split()
        if len(words) == 2 and words[0] == ".names" and words[1] not in used:
            continue
        kept.extend(block)
    return "\n".join(kept) + ("\n" if blif.endswith("\n") else "")


# The bit gates as BLIF models: both writers spell a gate as ``.subckt $_AND_ ...`` with no model
# (Yosys ``-icells``), which ``equiv-check`` cannot flatten, so tier T appends these definitions.
GATE_MODELS = """
.model $_NOT_
.inputs A
.outputs Y
.names A Y
0 1
.end
.model $_AND_
.inputs A B
.outputs Y
.names A B Y
11 1
.end
.model $_OR_
.inputs A B
.outputs Y
.names A B Y
1- 1
-1 1
.end
.model $_XOR_
.inputs A B
.outputs Y
.names A B Y
10 1
01 1
.end
.model $_XNOR_
.inputs A B
.outputs Y
.names A B Y
00 1
11 1
.end
.model $_MUX_
.inputs A B S
.outputs Y
.names A B S Y
1-0 1
-11 1
.end
"""


def with_gate_models(text: str) -> str:
    """``text`` (a BLIF) followed by the models of the bit gates."""
    return text.rstrip("\n") + "\n" + GATE_MODELS
