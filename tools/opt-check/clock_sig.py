"""clock_sig: the clock signature of an RTLIL text (2F Task 17, review C1).

Yosys's ``async2sync`` lowering puts every flip-flop on one implicit clock, so a refinement proof
through it cannot see a clock net or an edge that changed. ``clk2fflogic`` can, but its proof is
too weak for big designs (the shadow registers it adds match nothing in the other side). This module
makes the cheap proof exact where it can: when, in every module, all storage cells use ONE clock
(the same primary input and the same polarity) on both sides, the one-clock model is the real one,
so the ``async2sync`` proof plus equal signatures is a proof. Any other pair (several clocks in a
module, a clock that is not a primary input, a clock or an edge that differs between the sides)
needs the clock-aware ``clk2fflogic`` proof.

``signatures(text)`` maps a module name to its set of clock uses ``(pin, signal, polarity)``;
``single_clock(text)`` is True when every module has at most one use and it is a primary input.
"""

from __future__ import annotations

import re

MODULE_BLOCK = re.compile(r"^module (\\\S+)\s*\n(.*?)^end\s*$", re.M | re.S)
CELL_BLOCK = re.compile(r"^\s*cell (\S+) (\S+)\s*\n(.*?)^\s*end\s*$", re.M | re.S)
INPUT_WIRE = re.compile(r"^\s*wire(?: width \d+)?(?: offset -?\d+)?(?: upto)?(?: signed)? "
                        r"input \d+ (\\\S+)\s*$", re.M)
CONNECT = re.compile(r"^\s*connect (\\\w+) (.+?)\s*$", re.M)
PARAM = re.compile(r"^\s*parameter(?: signed)? (\\\w+) (.+?)\s*$", re.M)
GATE_STORAGE = re.compile(r"^\$_(?:DFF|DFFE|SDFF|SDFFE|SDFFCE|DFFSR|DFFSRE|ALDFF|ALDFFE|DLATCH|"
                          r"DLATCHSR|ADLATCH)_([PN])")
WORD_CLOCKS = {"$dff": "CLK", "$dffe": "CLK", "$sdff": "CLK", "$sdffe": "CLK", "$sdffce": "CLK",
               "$adff": "CLK", "$adffe": "CLK", "$aldff": "CLK", "$aldffe": "CLK",
               "$dffsr": "CLK", "$dffsre": "CLK", "$dlatch": "EN", "$adlatch": "EN",
               "$dlatchsr": "EN"}
MEMORY_TYPES = ("$mem", "$mem_v2")
Use = tuple[str, str, str]


def modules(text: str) -> dict[str, str]:
    """Module name to its body."""
    return {m.group(1): m.group(2) for m in MODULE_BLOCK.finditer(text)}


def cell_uses(kind: str, body: str) -> set[Use]:
    """The clock uses of one cell; empty for a cell without storage."""
    pins = dict(CONNECT.findall(body))
    params = dict(PARAM.findall(body))
    if kind in WORD_CLOCKS:
        pin = WORD_CLOCKS[kind]
        pol = params.get(f"\\{pin}_POLARITY", "?")
        return {(pin, pins.get(f"\\{pin}", "?"), pol)}
    gate = GATE_STORAGE.match(kind)
    if gate is not None:
        pin = "E" if "LATCH" in kind else "C"
        return {(pin, pins.get(f"\\{pin}", "?"), gate.group(1))}
    if kind in MEMORY_TYPES:
        return memory_uses(pins, params)
    return set()


def memory_uses(pins: dict[str, str], params: dict[str, str]) -> set[Use]:
    """A memory's ports: the signals of each port group's clock and the polarity/enable constants,
    as sets (opt may drop a port, never change what a kept one is clocked by)."""
    uses: set[Use] = set()
    for group in ("RD", "WR"):
        clk = pins.get(f"\\{group}_CLK", "")
        enable = params.get(f"\\{group}_CLK_ENABLE", "")
        if clk and set(enable.split("'")[-1]) != {"0"}:
            pol = "".join(sorted(set(params.get(f"\\{group}_CLK_POLARITY", "?").split("'")[-1])))
            signals = set(re.findall(r"\\\S+(?: \[\d+\])?|\d+'[01xz]+", clk.strip("{} ")))
            uses |= {(f"{group}_CLK", signal, pol) for signal in signals}
    return uses


def signatures(text: str) -> dict[str, set[Use]]:
    """Module name to the clock uses of its cells."""
    found: dict[str, set[Use]] = {}
    for name, body in modules(text).items():
        uses: set[Use] = set()
        for cell in CELL_BLOCK.finditer(body):
            uses |= cell_uses(cell.group(1), cell.group(3))
        found[name] = uses
    return found


def is_input(signal: str, body: str) -> bool:
    """True when ``signal`` (``\\clk`` or ``\\bus [3]``) names a primary input of the module."""
    base = signal.split(" ", 1)[0]
    return base in INPUT_WIRE.findall(body)


def single_clock(text: str) -> bool:
    """True when every module has at most one clock use and that use is a primary input."""
    uses_of = signatures(text)
    for name, body in modules(text).items():
        uses = uses_of[name]
        if len(uses) > 1 or any(not is_input(use[1], body) for use in uses):
            return False
    return True


def same_clocks(gold: str, gate: str) -> bool:
    """True when both sides are single-clock and every module has the same clock use."""
    return single_clock(gold) and single_clock(gate) and signatures(gold) == signatures(gate)
