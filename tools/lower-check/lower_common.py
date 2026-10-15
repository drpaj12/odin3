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


def read_front(files: Sequence[str], top: str | None, techlib: str | None) -> list[str]:
    """Odin III's front of every run: ``[read_techlib L;] read_verilog --nolatches FILE...;
    hierarchy`` (a ``.blif`` file is ``read_blif`` instead, and needs no hierarchy)."""
    if len(files) == 1 and files[0].endswith(".blif"):
        return [f"read_blif {quote(files[0])}"]
    return opt_equiv.read_commands(files, top, True, techlib)


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
    blocks = _blif_blocks(blif)
    used = _blif_uses(blocks)
    kept: list[str] = []
    for block in blocks:
        words = block[0].split()
        if len(words) == 2 and words[0] == ".names" and words[1] not in used:
            continue
        kept.extend(block)
    return "\n".join(kept) + ("\n" if blif.endswith("\n") else "")
