"""Parser for the log of Yosys 0.55's ``fsm`` pass (4.8 spec section 12, T-1 and T-3).

``parse_log(text)`` returns, per module, the state registers ``fsm_detect`` marked and rejected
(with the reasons Yosys prints), the extraction failures, and one :class:`Fsm` per FSM that
reached ``fsm_info``.  The facts are the ones the 4.8 acceptance test compares: state count,
state bits before and after recoding, encoding, reset state index, numbers of control inputs,
control outputs and transitions.

Log lines read (yosys passes/fsm/*.cc, 0.55)::

    Found FSM state register M.W.                      fsm_detect.cc
    Not marking M.W as FSM state register:             fsm_detect.cc (reason lines follow)
    Extracting FSM `\\W' from module `\\M'.              fsm_extract.cc
      fsm extraction failed: REASON.                   fsm_extract.cc
    Recoding FSM `CELL' from module `\\M' using `E' encoding:    fsm_recode.cc
    FSM `CELL' from module `M':  /  Information on FSM CELL (\\W):   fsm_info (FsmData::log_info)
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

FOUND = re.compile(r"^Found FSM state register (.+)\.$")
NOT_MARKING = re.compile(r"^Not marking (.+) as FSM state register:$")
EXTRACTING = re.compile(r"^Extracting FSM `\\?(.+)' from module `\\?(.+)'\.$")
FAILED = re.compile(r"^\s+fsm extraction failed: (.+)\.$")
RECODING = re.compile(r"^Recoding FSM `(.+)' from module `\\?(.+)' using `(.+)' encoding:$")
MAPPING = re.compile(r"^\s+mapping auto encoding to `([a-z-]+)` for this FSM\.$")
PACKED = re.compile(r"^\s+existing encoding is already a packed binary encoding\.$")
CODE_MAP = re.compile(r"^\s+([01x-]+) -> ([01x-]+)$")
INFO_HEAD = re.compile(r"^\s+Information on FSM (.+) \(\\?(.+)\):$")
FSM_OF = re.compile(r"^FSM `(.+)' from module `\\?(.+)':$")
COUNT = re.compile(
    r"^\s+[Nn]umber of (input signals|output signals|state bits):\s+(\d+)$")
ENC_LINE = re.compile(r"^\s+\d+:\s+\d+'([01x-]+)(\s+<RESET STATE>)?$")
TRANS_LINE = re.compile(r"^\s+\d+:\s+(-?\d+) (\d+'[01x-]+)\s+->\s+(-?\d+) (\d+'[01x-]+)$")


@dataclass
class Fsm:
    """The facts of one FSM, as ``fsm_info`` reports them after ``fsm_recode``."""

    module: str
    wire: str
    cell: str
    states: int = 0
    bits_before: int = 0
    bits_after: int = 0
    encoding: str = "auto"
    reset_index: int = -1
    inputs: int = 0
    outputs: int = 0
    transitions: int = 0
    codes: tuple[str, ...] = ()
    table: tuple[tuple[int, str, int, str], ...] = ()


@dataclass
class ModuleFacts:
    """Everything the log says about one module."""

    marked: list[str] = field(default_factory=list)
    rejected: dict[str, list[str]] = field(default_factory=dict)
    failures: list[tuple[str, str]] = field(default_factory=list)
    fsms: list[Fsm] = field(default_factory=list)


@dataclass
class _Recode:
    encoding: str
    bits_before: int = 0
    bits_after: int = 0
    packed: bool = False


def _split_reg(name: str) -> tuple[str, str]:
    """``module.wire`` of a detect line; a module name has no dot, a wire name may."""
    module, _, wire = name.partition(".")
    return module, wire


def _parse_recode(lines: list[str], start: int) -> tuple[_Recode, int]:
    m = RECODING.match(lines[start])
    assert m is not None
    rec = _Recode(encoding=m.group(3))
    i = start + 1
    while i < len(lines):
        line = lines[i]
        if (mm := MAPPING.match(line)) is not None:
            rec.encoding = mm.group(1)
        elif PACKED.match(line):
            rec.packed = True
        elif (cm := CODE_MAP.match(line)) is not None:
            rec.bits_before, rec.bits_after = len(cm.group(1)), len(cm.group(2))
        elif not line.startswith("  "):
            break
        i += 1
    return rec, i


@dataclass
class InfoBlock:
    """What one ``FsmData::log_info`` block holds (Yosys's text, and Odin III's copy of it)."""

    counts: dict[str, int] = field(default_factory=dict)
    codes: list[str] = field(default_factory=list)
    reset: int = -1
    table: list[tuple[int, str, int, str]] = field(default_factory=list)


def parse_info_block(lines: list[str], start: int, stop_prefix: str = "-----") -> InfoBlock:
    """The counts, state codes, reset state and transition table of the block after ``start``."""
    info = InfoBlock()
    i = start + 1
    while i < len(lines) and not lines[i].startswith(stop_prefix):
        line = lines[i]
        if (c := COUNT.match(line)) is not None:
            info.counts[c.group(1).lower()] = int(c.group(2))
        elif (e := ENC_LINE.match(line)) is not None:
            if e.group(2):
                info.reset = len(info.codes)
            info.codes.append(e.group(1))
        elif (t := TRANS_LINE.match(line)) is not None:
            info.table.append((int(t.group(1)), t.group(2), int(t.group(3)), t.group(4)))
        i += 1
    return info


def parse_log(text: str) -> dict[str, ModuleFacts]:
    """Parse a Yosys 0.55 log that ran ``fsm`` (or its detect/extract/recode/info stages)."""
    lines = text.splitlines()
    mods: dict[str, ModuleFacts] = {}
    recodes: dict[str, _Recode] = {}
    extracting: tuple[str, str] | None = None
    fsm_module: dict[str, str] = {}
    i = 0
    while i < len(lines):
        line = lines[i]
        if (m := FOUND.match(line)) is not None:
            mod, wire = _split_reg(m.group(1))
            mods.setdefault(mod, ModuleFacts()).marked.append(wire)
        elif (m := NOT_MARKING.match(line)) is not None:
            mod, wire = _split_reg(m.group(1))
            reasons: list[str] = []
            while i + 1 < len(lines) and lines[i + 1].startswith("    "):
                i += 1
                reasons.append(lines[i].strip().rstrip("."))
            mods.setdefault(mod, ModuleFacts()).rejected[wire] = reasons
        elif (m := EXTRACTING.match(line)) is not None:
            extracting = (m.group(2), m.group(1))
        elif (m := FAILED.match(line)) is not None and extracting is not None:
            mods.setdefault(extracting[0], ModuleFacts()).failures.append(
                (extracting[1], m.group(1)))
        elif (m := RECODING.match(line)) is not None:
            rec, nxt = _parse_recode(lines, i)
            recodes[m.group(1)] = rec
            fsm_module[m.group(1)] = m.group(2)
            i = nxt - 1
        elif (m := FSM_OF.match(line)) is not None:
            fsm_module[m.group(1)] = m.group(2)
        elif (m := INFO_HEAD.match(line)) is not None:
            cell, wire = m.group(1), m.group(2)
            info = parse_info_block(lines, i)
            rec = recodes.get(cell, _Recode(encoding="auto"))
            bits = info.counts.get("state bits", 0)
            mod = fsm_module.get(cell, "")
            enc = "binary" if rec.packed else rec.encoding
            mods.setdefault(mod, ModuleFacts()).fsms.append(Fsm(
                module=mod, wire=wire, cell=cell, states=len(info.codes),
                bits_before=rec.bits_before or bits, bits_after=rec.bits_after or bits,
                encoding=enc, reset_index=info.reset, inputs=info.counts.get("input signals", 0),
                outputs=info.counts.get("output signals", 0), transitions=len(info.table),
                codes=tuple(info.codes), table=tuple(info.table)))
        i += 1
    return mods
