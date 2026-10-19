"""icells-check: the per-cell signature of an RTLIL netlist (5.2 review M2).

``signature(il_text)`` maps every module to the multiset of its cells and nets after four rounds of
colour refinement (the Weisfeiler-Leman scheme): a cell starts from its type, its parameters and
its port widths; a net from the public names of its class (nets joined by module-level
``connect`` lines are one net) or, with none, from nothing. Each round mixes in the colours around
it. Two netlists that differ in a cell type, a parameter, a port width or the wiring have different
signatures with overwhelming probability, whatever the generated names and the order of the cells
(the writer and the reader name generated objects anew on every pass), and an alias wire that only
adds a name to an existing net changes nothing.
"""

from __future__ import annotations

import hashlib
import re
from collections import Counter

ROUNDS = 4
GENERATED = re.compile(r"^\\?\$")
TOKEN = re.compile(r"\{|\}|\\?[^\s{}\[\]]+|\[[0-9:]+\]")
CONST = re.compile(r"^(\d+)'([01xzm-]*)$")

Bit = tuple[str, int]
Pin = tuple[str, int, str]


def _digest(*parts: object) -> str:
    return hashlib.sha256(repr(parts).encode()).hexdigest()[:16]


class Module:
    """The wires, cells and aliases of one RTLIL module."""

    def __init__(self, name: str) -> None:
        self.name = name
        self.widths: dict[str, int] = {}
        self.cells: list[tuple[str, list[str], list[tuple[str, str]]]] = []
        self.aliases: list[tuple[str, str]] = []


def parse(il: str) -> list[Module]:
    """The modules of an RTLIL text (wires, cells with parameters and connections, aliases)."""
    modules: list[Module] = []
    cell: tuple[str, list[str], list[tuple[str, str]]] | None = None
    for raw in il.splitlines():
        line = raw.strip()
        if line.startswith("module "):
            modules.append(Module(line.split(None, 1)[1]))
        elif not modules:
            continue
        elif line.startswith("wire "):
            words = line.split()
            width = int(words[words.index("width") + 1]) if "width" in words else 1
            modules[-1].widths[words[-1]] = width
        elif line.startswith("cell "):
            cell = (line.split()[1], [], [])
        elif line == "end" and cell is not None:
            modules[-1].cells.append(cell)
            cell = None
        elif cell is not None and line.startswith("parameter "):
            cell[1].append(line.split(None, 1)[1])
        elif cell is not None and line.startswith("connect "):
            port, sig = line.split(None, 2)[1:]
            cell[2].append((port, sig))
        elif line.startswith("connect "):
            lhs, rhs = line.split(None, 2)[1:]
            modules[-1].aliases.append((lhs, rhs))
    return modules


def _elements(sig: str) -> list[tuple[str, str]]:
    """The elements of a sigspec, most significant first: (wire or constant, slice text)."""
    tokens = TOKEN.findall(sig)
    out: list[tuple[str, str]] = []
    for token in tokens:
        if token in ("{", "}"):
            continue
        if token.startswith("[") and out:
            out[-1] = (out[-1][0], token)
        else:
            out.append((token, ""))
    return out


def bits(sig: str, widths: dict[str, int]) -> list[Bit]:
    """The bits of a sigspec, least significant first: ``(wire, index)`` or ``("'c", 0)``."""
    result: list[Bit] = []
    for name, part in reversed(_elements(sig)):
        found = CONST.match(name)
        if found:
            digits = found.group(2).rjust(int(found.group(1)), found.group(2)[:1] or "0")
            result += [("'" + chr_, 0) for chr_ in reversed(digits[-int(found.group(1)) :])]
            continue
        if part:
            high, _, low = part[1:-1].partition(":")
            span = range(int(low or high), int(high) + 1)
        else:
            span = range(widths.get(name, 1))
        result += [(name, idx) for idx in span]
    return result


class _Union:
    def __init__(self) -> None:
        self.parent: dict[Bit, Bit] = {}

    def find(self, item: Bit) -> Bit:
        self.parent.setdefault(item, item)
        while self.parent[item] != item:
            self.parent[item] = self.parent[self.parent[item]]
            item = self.parent[item]
        return item

    def join(self, one: Bit, two: Bit) -> None:
        self.parent[self.find(one)] = self.find(two)


def _net_labels(mod: Module, union: _Union) -> dict[Bit, str]:
    """Net -> the digest of the public (non-generated) names in its class."""
    names: dict[Bit, list[str]] = {}
    for name, width in mod.widths.items():
        if not GENERATED.match(name):
            for idx in range(width):
                names.setdefault(union.find((name, idx)), []).append(f"{name}[{idx}]")
    return {net: _digest(sorted(items)) for net, items in names.items()}


def _module_signature(mod: Module) -> Counter[str]:
    union = _Union()
    for lhs, rhs in mod.aliases:
        for one, two in zip(bits(lhs, mod.widths), bits(rhs, mod.widths), strict=False):
            union.join(one, two)
    labels = _net_labels(mod, union)
    pins: list[list[Pin]] = []
    cell_color: list[str] = []
    net_color: dict[str, str] = {}
    for ctype, params, conns in mod.cells:
        cell_pins: list[Pin] = []
        for port, sig in conns:
            for idx, bit in enumerate(bits(sig, mod.widths)):
                const = bit[0].startswith("'")
                net = bit if const else union.find(bit)
                cell_pins.append((port, idx, repr(net)))
                net_color[repr(net)] = "const" + bit[0] if const else labels.get(net, "")
        pins.append(cell_pins)
        widths = sorted(Counter(port for port, _, _ in cell_pins).items())
        cell_color.append(_digest(ctype, sorted(params), widths))
    for _ in range(ROUNDS):
        net_color, cell_color = _refine(pins, net_color, cell_color)
    return Counter(["cell:" + c for c in cell_color] + ["net:" + n for n in net_color.values()])


def _refine(
    pins: list[list[Pin]], net_color: dict[str, str], cell_color: list[str]
) -> tuple[dict[str, str], list[str]]:
    """One colour-refinement round: nets from the cells around them, cells from their nets."""
    around: dict[str, list[tuple[str, str, int]]] = {}
    for index, cell_pins in enumerate(pins):
        for port, idx, net in cell_pins:
            around.setdefault(net, []).append((cell_color[index], port, idx))
    new_net = {net: _digest(net_color[net], sorted(items)) for net, items in around.items()}
    new_cell = [
        _digest(cell_color[i], sorted((port, idx, net_color[net]) for port, idx, net in cell_pins))
        for i, cell_pins in enumerate(pins)
    ]
    return new_net, new_cell


def signature(il: str) -> dict[str, Counter[str]]:
    """Module name -> the multiset of cell and net colours."""
    return {mod.name: _module_signature(mod) for mod in parse(il)}


def cell_count(sig: dict[str, Counter[str]]) -> int:
    """The number of cells in a signature (0 means nothing was parsed)."""
    return sum(n for counter in sig.values() for key, n in counter.items() if key[:5] == "cell:")
