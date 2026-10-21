"""re_round: one round of the reverse-engineering loop of docs/RE-ARCHITECTURE.md section 4.

Reads a BLIF whose signals have no useful names, guesses a ripple-carry adder in it by a
structural rule, proves the guess with `cone_check`, replaces it by one `$add` with `raise` and
writes Verilog (and, with --json, the design as JSON). It uses only the public ABI through
odin3.py. Level 0 (the BLIF as read) is never edited: the round works on `Design.copy()`.

The rule: the logic is $sop nodes of at most 3 inputs. A sum cell is an XOR or XNOR (of 2 inputs
at bit 0, else of 3); a carry cell is a majority (an AND at bit 0) with any inversions. Bit 0 is
a sum and a carry cell on the same two nets; the carry net feeds the next sum cell, whose other
two inputs are bit i of the operands, and the next carry cell is the majority cell on the same
three nets. The chain ends when no sum cell reads the carry. The rule only guesses: `cone_check`
decides, so a wrong guess (or inverted carry polarity that is not an adder) is reported, never
raised.

Run: python3 plugins/python/re_round.py IN.blif OUT.v [--json OUT.json] [--timeout S]
     [--techlib LIB]
Exit 0 when the adder is found, proved and raised; 1 when none is found or the proof fails.
"""

from __future__ import annotations

import argparse
import itertools
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from odin3 import Design, Module, Net, Node, default

GROUP = "add"  # the attribute key `grp`: its value marks the guessed cone
MAJ3 = frozenset({3, 5, 6, 7})  # minterms of the 3-input majority (bit i of x = input i)
PARITY3 = (frozenset({1, 2, 4, 7}), frozenset({0, 3, 5, 6}))
PARITY2 = (frozenset({1, 2}), frozenset({0, 3}))
FULL3 = frozenset(range(8))


@dataclass(frozen=True)
class Cell:
    """A logic cell: its input net IDs, its output net ID and its function."""

    node: Node
    ins: tuple[int, ...]
    out: int
    on: frozenset[int]  # the minterms that give 1


class RoundError(Exception):
    """The round stopped: no adder, or the proof did not hold."""


def minterms(cover: str, width: int) -> frozenset[int]:
    """The on-set of a $sop COVER ('pattern 1' lines; '-' is any value)."""
    out: set[int] = set()
    for line in cover.splitlines():
        pattern, _, value = line.partition(" ")
        if value != "1":
            raise RoundError("only on-set covers are handled")
        for x in range(1 << width):
            if all(c == "-" or int(c) == (x >> i) & 1 for i, c in enumerate(pattern)):
                out.add(x)
    return frozenset(out)


def cells(module: Module) -> list[Cell]:
    """The live $sop nodes of at most 3 inputs, in node order."""
    found: list[Cell] = []
    for node in module.nodes():
        if node.type_name != "$sop":
            continue
        ports = node.ports()
        width = len(ports[0].pins)
        outs = ports[1].pins[0].net()
        nets = [pin.net() for pin in ports[0].pins]
        if width > 3 or outs is None or any(n is None for n in nets):
            continue
        ins = tuple(n.id for n in nets if n is not None)
        found.append(Cell(node, ins, outs.id, minterms(str(node.params()["COVER"]), width)))
    return found


def is_sum(cell: Cell) -> bool:
    """XOR or XNOR of two inputs, or of three."""
    return cell.on in (PARITY3 if len(cell.ins) == 3 else PARITY2 if len(cell.ins) == 2 else ())


def is_carry(cell: Cell) -> bool:
    """Majority of three with any input and output inversion; two inputs: an AND with any
    inversion (one minterm) or its complement (three)."""
    if len(cell.ins) == 2:
        return len(cell.on) in (1, 3)
    if len(cell.ins) != 3:
        return False
    for mask, flip in itertools.product(range(8), (frozenset(), FULL3)):
        if cell.on == frozenset(x for x in range(8) if (x ^ mask) in MAJ3) ^ flip:
            return True
    return False


@dataclass
class Adder:
    """The guess: the cells of the cone and the net each operand and sum bit stands for."""

    cells: list[Cell]
    binding: dict[int, str]  # net ID -> template port bit

    @property
    def width(self) -> int:
        return sum(1 for port in self.binding.values() if port.startswith("Y["))


def chain_from(start: Cell, carry: Cell, all_cells: Sequence[Cell]) -> Adder | None:
    """Follow the carry from bit 0 (cells `start`, `carry`); None for a chain of one bit."""
    group = [start, carry]
    binding = {start.ins[0]: "A[0]", start.ins[1]: "B[0]", start.out: "Y[0]"}
    c = carry.out
    while True:
        nxt = next((x for x in all_cells if is_sum(x) and len(x.ins) == 3 and c in x.ins
                    and x not in group), None)
        if nxt is None:
            break
        bit = sum(1 for p in binding.values() if p.startswith("Y["))
        a, b = (n for n in nxt.ins if n != c)
        binding.update({a: f"A[{bit}]", b: f"B[{bit}]", nxt.out: f"Y[{bit}]"})
        group.append(nxt)
        nets = set(nxt.ins)
        cout = next((x for x in all_cells if is_carry(x) and len(x.ins) == 3
                     and set(x.ins) == nets and x not in group), None)
        if cout is None:
            break
        group.append(cout)
        c = cout.out
    return Adder(group, binding) if len(group) > 2 else None


def find_adder(module: Module) -> Adder | None:
    """The longest ripple chain the rule finds in the module, None when there is none."""
    all_cells = cells(module)
    best: Adder | None = None
    for start in (x for x in all_cells if len(x.ins) == 2 and is_sum(x)):
        for carry in (x for x in all_cells if len(x.ins) == 2 and is_carry(x)
                      and set(x.ins) == set(start.ins)):
            found = chain_from(start, carry, all_cells)
            if found is not None and (best is None or found.width > best.width):
                best = found
    return best


def mark(design: Design, module: Module, adder: Adder) -> None:
    """Attribute grp=add on the cone's nodes; odin3:bind on its boundary nets."""
    for cell in adder.cells:
        cell.node.set_attr("grp", GROUP)
    for net_id, port in adder.binding.items():
        Net(design, module.id, net_id).set_attr("odin3:bind", port)


def round_once(level0: Design, timeout: int = 60) -> Design:
    """Level 1 from level 0: guess, mark, prove, raise. RoundError when it cannot."""
    level1 = level0.copy()
    module = level1.top()
    if module is None:
        raise RoundError("the design has no top module")
    adder = find_adder(module)
    if adder is None:
        raise RoundError("no adder found by the ripple-carry rule")
    width = adder.width
    print(f"re_round: guess: {width}-bit adder, {len(adder.cells)} cells")
    mark(level1, module, adder)
    args = (f"-attr grp={GROUP} -cell $add -param A_WIDTH={width} -param B_WIDTH={width} "
            f"-param Y_WIDTH={width}")
    level1.run_pass("cone_check", f"{args} -timeout {timeout}")
    verdict = module.attr("odin3:cone_check")
    print(f"re_round: cone_check: {verdict}"
          + (f", counterexample {module.attr('odin3:cone_cex')}" if verdict == "differs" else ""))
    if verdict != "equivalent":
        raise RoundError(f"the guess was not proved ({verdict})")
    level1.run_pass("raise", args)
    print(f"re_round: raise: {len(adder.cells)} cells replaced by one $add")
    return level1


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("blif", type=Path)
    parser.add_argument("verilog", type=Path)
    parser.add_argument("--json", type=Path, help="also write the raised design as JSON")
    parser.add_argument("--timeout", type=int, default=60, help="cone_check seconds")
    parser.add_argument("--techlib", type=Path, action="append", default=[])
    args = parser.parse_args(argv)
    try:
        default().load_abc()  # cone_check needs the linked ABC
        with (
            Design.read_blif(args.blif, techlibs=args.techlib) as level0,
            round_once(level0, args.timeout) as level1,
        ):
            level1.run_pass("write_verilog", f'"{args.verilog}"')
            if args.json:
                level1.run_pass("write_json", f'"{args.json}" --odin3')
    except RoundError as exc:
        print(f"re_round: {exc}")
        return 1
    print(f"re_round: wrote {args.verilog}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
