"""Parser for the log lines of Odin III's ``fsm`` pass (4.8 spec section 16, I-1), in the form of
``yosys_log``: the same ``ModuleFacts`` and ``Fsm`` records, so the two logs compare directly.

Lines read (``odin3: <level>: `` in front of each is ignored)::

    fsm: found state register 'M.W'
    fsm: not marking 'M.W': REASON
    fsm: extraction of 'M.W' failed: REASON
    fsm: recoding 'M.W' using 'ATTR' encoding
    fsm: recoding 'M.W' with ENC encoding (S states, B bits, was W)
    fsm: 'M.W': existing encoding is already a packed binary encoding
    fsm: 'M.W': nothing to do for encoding 'ENC'
    fsm: information on FSM 'M.W':    then the table lines (``fsm:   ...``)

``M.W`` splits at its last dot (a module name may hold dots, a wire name of a register does not).
"""

from __future__ import annotations

import re

from yosys_log import Fsm, ModuleFacts, parse_info_block

LEVEL = re.compile(r"^odin3: (?:info|warning|error|debug): ")
FOUND = re.compile(r"^fsm: found state register '(.+)'$")
NOT_MARKING = re.compile(r"^fsm: not marking '(.+)': (.+)$")
FAILED = re.compile(r"^fsm: extraction of '(.+)' failed: (.+)$")
RECODING_USING = re.compile(r"^fsm: recoding '(.+)' using '(.+)' encoding$")
RECODED = re.compile(
    r"^fsm: recoding '(.+)' with (\S+) encoding \((\d+) states, (\d+) bits, was (\d+)\)$")
PACKED = re.compile(r"^fsm: '(.+)': existing encoding is already a packed binary encoding$")
NOTHING = re.compile(r"^fsm: '(.+)': nothing to do for encoding '(.+)'$")
INFO_HEAD = re.compile(r"^fsm: information on FSM '(.+)':$")

REASON_TEXT = {
    "connected to a module port": "Register is connected to module port",
    "users do not benefit from recoding": "Users of register don't seem to benefit from recoding",
    "has an initialization value": "Register has an initialization value",
    "seems self-resetting": "Circuit seems to be self-resetting",
}


def _split(label: str) -> tuple[str, str]:
    module, _, wire = label.rpartition(".")
    return module, wire


def _strip(text: str) -> list[str]:
    return [LEVEL.sub("", line) for line in text.splitlines()]


def parse_log(text: str) -> dict[str, ModuleFacts]:
    """Parse the log of an ``odin3`` run that ran ``fsm`` (or ``fsm_detect``)."""
    lines = _strip(text)
    mods: dict[str, ModuleFacts] = {}
    recoded: dict[str, tuple[str, int, int]] = {}
    attr: dict[str, str] = {}
    for i, line in enumerate(lines):
        if (m := FOUND.match(line)) is not None:
            mod, wire = _split(m.group(1))
            mods.setdefault(mod, ModuleFacts()).marked.append(wire)
        elif (m := NOT_MARKING.match(line)) is not None:
            mod, wire = _split(m.group(1))
            facts = mods.setdefault(mod, ModuleFacts())
            facts.rejected.setdefault(wire, []).append(REASON_TEXT.get(m.group(2), m.group(2)))
        elif (m := FAILED.match(line)) is not None:
            mod, wire = _split(m.group(1))
            mods.setdefault(mod, ModuleFacts()).failures.append((wire, m.group(2)))
        elif (m := RECODING_USING.match(line)) is not None:
            attr[m.group(1)] = m.group(2)
        elif (m := RECODED.match(line)) is not None:
            recoded[m.group(1)] = (m.group(2), int(m.group(5)), int(m.group(4)))
        elif (m := PACKED.match(line)) is not None:
            recoded[m.group(1)] = ("binary", 0, 0)
        elif (m := NOTHING.match(line)) is not None:
            recoded[m.group(1)] = (m.group(2), 0, 0)
        elif (m := INFO_HEAD.match(line)) is not None:
            _add_fsm(mods, lines, i, (m.group(1), recoded.get(m.group(1)), attr.get(m.group(1))))
    return mods


def _add_fsm(mods: dict[str, ModuleFacts], lines: list[str], at: int,
             what: tuple[str, tuple[str, int, int] | None, str | None]) -> None:
    label, recode, attribute = what
    body: list[str] = []
    for line in lines[at + 1:]:
        if not line.startswith("fsm:  "):
            break
        body.append(line[len("fsm:"):])
    info = parse_info_block(body, -1, stop_prefix="\0")
    bits = info.counts.get("state bits", 0)
    enc, before, after = recode if recode is not None else (attribute or "auto", 0, 0)
    mod, wire = _split(label)
    mods.setdefault(mod, ModuleFacts()).fsms.append(Fsm(
        module=mod, wire=wire, cell=label, states=len(info.codes), bits_before=before or bits,
        bits_after=after or bits, encoding=enc, reset_index=info.reset,
        inputs=info.counts.get("input signals", 0), outputs=info.counts.get("output signals", 0),
        transitions=len(info.table), codes=tuple(info.codes), table=tuple(info.table)))
