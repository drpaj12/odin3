"""il_order: the cells of an RTLIL file in operand-first order (3G spec WR-7, amended 2026-10-19).

Yosys's ``wreduce`` visits cells in the order their names were interned, which for its own front
end is operand before consumer; Odin III's ``write_rtlil`` writes node-ID order, and the elaborator
numbers a consumer first. ``opt-diff`` hands Yosys an Odin III RTLIL file, so it puts the cells of
that file in the order ``src/passes/opt/wreduce_order.c`` ranks them (the unit test
``test_il_celltypes`` keeps the cell table ``il_celltypes.txt`` equal to the registry and to the C
file). The walk, bit for bit as in C:

* nodes are entered in file order; the driver of each input pin is placed before the node;
* ports are read in table order (``$mux``: S, B, A), bits LSB first (the last name of a ``{ ... }``
  concatenation is bit 0), slices and constants resolved to bits;
* the first driver of a bit (file order) is the one followed, after the module-level ``connect``
  lines (a left-hand bit is its right-hand bit);
* only an anonymous bit is followed: one that no public wire (``\\name``) stands for; a named
  net is a statement boundary;
* the outputs of storage cells (``seq`` in the table) are not followed; a loop is cut where the
  walk meets a cell already entered.

Cells are placed in the slots the cell blocks occupied; every other line, and the bytes of every
line (line ends included), stay as they were, with one exception: Yosys interns names over the
whole design, so a cell name that an earlier module of the file already has (Odin III's ``$c<ID>``
repeat from module to module) would take the order of its first use. A repeated name is made
unique (``$c150`` in the third module becomes ``$c150$m3``), so that the order of a module is its
own, as in Yosys's front end, whose names are unique. A cell type or a port the table does not have,
an instance of a module the file does not define, a signal this tool does not read and a result
that is not a permutation of the input raise ``OrderError`` (exit 2): the tool never guesses.

Only ``opt-diff`` uses it; ``opt-rules`` hands Yosys the file as Odin III wrote it.

Usage: ``il_order.py IN.il OUT.il``.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

TABLE = Path(__file__).resolve().parent / "il_celltypes.txt"
_TOKEN = re.compile(r"\{|\}|\[[^\]]*\]|[\\$][^\s{}\[\]]+|\d+'[01xzmu-]*")
_WIRE_FLAGS = {"signed"}

Bit = tuple[str, int]  # (wire name, index)


class OrderError(Exception):
    """Something this tool does not read: the caller stops (exit 2)."""


@dataclass(frozen=True)
class CellType:
    seq: bool
    ports: tuple[tuple[str, str], ...]  # (name, "i" | "o" | "io") in registry order
    walk: tuple[str, ...]  # the port names in walk order


def load_table(path: Path = TABLE) -> dict[str, CellType]:
    """The rows of ``il_celltypes.txt`` (Yosys-only rows included)."""
    table: dict[str, CellType] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        words = raw.split()
        if not words or words[0].startswith("#"):
            continue
        if words[0] == "yosys":
            words = words[1:]
        name, kind, rest = words[0], words[1], words[2:]
        walk = [w[5:] for w in rest if w.startswith("walk=")]
        ports = tuple((p.rsplit(":", 1)[0], p.rsplit(":", 1)[1]) for p in rest
                      if not p.startswith("walk="))
        order = tuple(walk[0].split(",")) if walk else tuple(p for p, _ in ports)
        table[name] = CellType(kind == "seq", ports, order)
    return table


@dataclass
class Module:
    name: str
    widths: dict[str, int] = field(default_factory=dict)
    layouts: dict[str, tuple[int, bool]] = field(default_factory=dict)  # (offset, upto)
    port_dirs: list[tuple[int, str, str]] = field(default_factory=list)  # (number, name, dir)


@dataclass
class Item:
    """One slot of a module: a line that stays, or a cell block that may move."""

    lines: list[str]
    cell: bool


def _body(line: str) -> str:
    return line.rstrip("\r\n")


@dataclass(frozen=True)
class Wire:
    name: str
    width: int
    direction: str | None
    number: int
    offset: int
    upto: bool


def parse_wire(line: str) -> Wire:
    """The fields of a ``wire`` line."""
    words = _body(line).split()[1:]
    name, width, direction, number, i = words[-1], 1, None, 0, 0
    offset, upto = 0, False
    while i < len(words) - 1:
        word = words[i]
        if word == "width":
            width = int(words[i + 1])
            i += 2
        elif word in ("input", "output", "inout"):
            direction, number = word, int(words[i + 1])
            i += 2
        elif word == "offset":
            offset = int(words[i + 1])
            i += 2
        elif word == "upto":
            upto = True
            i += 1
        elif word in _WIRE_FLAGS:
            i += 1
        else:
            raise OrderError(f"wire line not read: {_body(line)!r}")
    return Wire(name, width, direction, number, offset, upto)


def split_items(lines: list[str]) -> list[Item]:
    """The lines of one module (between ``module`` and ``end``) as lines and cell blocks. A block
    is the attribute lines before a ``cell`` (two-space indent) and the cell up to its ``  end``;
    a ``process`` block stays whole as a plain item."""
    items: list[Item] = []
    block: list[str] | None = None
    block_is_cell = False
    pending: list[str] = []
    for line in lines:
        text = _body(line)
        if block is not None:
            block.append(line)
            if text == "  end":
                items.append(Item(block, block_is_cell))
                block = None
        elif text.startswith("  attribute "):
            pending.append(line)
        elif text.startswith("  cell ") or text.startswith("  process "):
            block, block_is_cell = [*pending, line], text.startswith("  cell ")
            pending = []
        else:
            items.extend(Item([p], False) for p in pending)
            pending = []
            items.append(Item([line], False))
    if block is not None:
        raise OrderError("cell or process without 'end'")
    items.extend(Item([p], False) for p in pending)
    return items


class Signals:
    """A module's signal bits: wire widths, the module-level ``connect`` aliases, public wires."""

    def __init__(self, widths: dict[str, int], layouts: dict[str, tuple[int, bool]]) -> None:
        self.widths = widths
        self.layouts = layouts
        self.alias: dict[Bit, Bit] = {}
        self.named: set[Bit] = set()

    def bits(self, text: str) -> list[Bit | None]:
        """LSB-first bits of a signal text; ``None`` is a constant bit."""
        tokens = _TOKEN.findall(text)
        if "".join(tokens) != "".join(text.split()):
            raise OrderError(f"signal not read: {text!r}")
        value, at = self._parse(tokens, 0)
        if at != len(tokens):
            raise OrderError(f"signal not read: {text!r}")
        return value

    def _parse(self, tokens: list[str], at: int) -> tuple[list[Bit | None], int]:
        token = tokens[at]
        if token == "{":  # the first name is the MSB: the bits are the names in reverse
            out: list[Bit | None] = []
            at += 1
            parts: list[list[Bit | None]] = []
            while at < len(tokens) and tokens[at] != "}":
                part, at = self._parse(tokens, at)
                parts.append(part)
            if at >= len(tokens):
                raise OrderError("unclosed '{'")
            for part in reversed(parts):
                out.extend(part)
            return out, at + 1
        if "'" in token:
            return [None] * int(token.split("'")[0]), at + 1
        if token[0] not in "\\$":
            raise OrderError(f"signal token not read: {token!r}")
        if token not in self.widths:
            raise OrderError(f"wire '{token}' is not declared")
        lo, hi = 0, self.widths[token] - 1
        at += 1
        if at < len(tokens) and tokens[at].startswith("["):
            lo, hi = self._slice(token, tokens[at])
            at += 1
        return [(token, i) for i in range(lo, hi + 1)], at

    def _index(self, name: str, number: int) -> int:
        """The bit index (0 = LSB side of the storage) of declared bit ``number`` of a wire."""
        offset, upto = self.layouts[name]
        width = self.widths[name]
        index = width - 1 - (number - offset) if upto else number - offset
        if not 0 <= index < width:
            raise OrderError(f"bit {number} is outside wire '{name}'")
        return index

    def _slice(self, name: str, text: str) -> tuple[int, int]:
        inner = text[1:-1].split(":")
        first, last = self._index(name, int(inner[0])), self._index(name, int(inner[-1]))
        return min(first, last), max(first, last)

    def add_connect(self, lhs: str, rhs: str) -> None:
        left, right = self.bits(lhs), self.bits(rhs)
        if len(left) != len(right):
            raise OrderError(f"connect of unequal widths: {lhs!r} {rhs!r}")
        for dst, src in zip(left, right, strict=True):
            if dst is not None and src is not None:
                self.alias.setdefault(dst, src)

    def resolve(self, bit: Bit) -> Bit:
        seen = {bit}
        while bit in self.alias and self.alias[bit] not in seen:
            bit = self.alias[bit]
            seen.add(bit)
        return bit

    def mark_named(self) -> None:
        for name, width in self.widths.items():
            if name.startswith("\\"):
                self.named.update(self.resolve((name, i)) for i in range(width))


def parse_modules(text_lines: list[str]) -> dict[str, Module]:
    """The wire widths and the port directions of every module of the file."""
    modules: dict[str, Module] = {}
    current: Module | None = None
    for line in text_lines:
        body = _body(line)
        if body.startswith("module "):
            current = modules.setdefault(body.split()[1], Module(body.split()[1]))
        elif current is not None and body.startswith("  wire "):
            wire = parse_wire(line)
            current.widths[wire.name] = wire.width
            current.layouts[wire.name] = (wire.offset, wire.upto)
            if wire.direction is not None:
                current.port_dirs.append((wire.number, wire.name, wire.direction))
    return modules


def cell_ports(kind: str, table: dict[str, CellType], modules: dict[str, Module]
               ) -> tuple[bool, list[tuple[str, str]], tuple[str, ...]]:
    """(storage, ports with direction, walk) of a cell type; ``OrderError`` if unknown."""
    if kind in table:
        ctype = table[kind]
        return ctype.seq, list(ctype.ports), ctype.walk
    if kind.startswith("\\") and kind in modules:
        pdirs = sorted(modules[kind].port_dirs)
        # a module input is read by the instance; a module output is driven by it
        ports = [(n.lstrip("\\"), {"input": "i", "output": "o", "inout": "io"}[d])
                 for _, n, d in pdirs]
        return False, ports, tuple(n for n, _ in ports)
    raise OrderError(f"cell type {kind} is not in il_celltypes.txt and not a module of the file")


@dataclass
class CellInfo:
    seq: bool
    reads: list[list[Bit | None]]  # bits of every reading pin, in walk order
    drives: list[Bit | None]
    items: int = 0


def read_cell(block: list[str], sig: Signals, table: dict[str, CellType],
              modules: dict[str, Module]) -> CellInfo:
    start = next(i for i, line in enumerate(block) if _body(line).startswith("  cell "))
    kind = _body(block[start]).split()[1]
    seq, ports, walk = cell_ports(kind, table, modules)
    known = {n: d for n, d in ports}
    conns: dict[str, str] = {}
    for line in block[start + 1:]:
        text = _body(line)
        if text.startswith("    connect "):
            words = text.split(None, 2)
            port = words[1].lstrip("\\")
            if port not in known:
                raise OrderError(f"cell type {kind}: port {port} is not in the table")
            conns[port] = words[2]
    reads: list[list[Bit | None]] = []
    drives: list[Bit | None] = []
    for port in walk:
        if port in conns:
            bits = sig.bits(conns[port])
            if known[port] in ("i", "io"):
                reads.append(bits)
            if known[port] in ("o", "io"):
                drives.extend(bits)
    return CellInfo(seq, reads, drives)


def order_cells(blocks: list[list[str]], sig: Signals, table: dict[str, CellType],
                modules: dict[str, Module]) -> list[int]:
    """The indexes of ``blocks`` in operand-first post-order (see the module docstring)."""
    infos = [read_cell(b, sig, table, modules) for b in blocks]
    driver: dict[Bit, int] = {}
    for idx, info in enumerate(infos):
        for bit in info.drives:
            if bit is not None:
                driver.setdefault(sig.resolve(bit), idx)
    deps: list[list[int]] = []
    for idx, info in enumerate(infos):
        found: list[int] = []
        for bits in info.reads:
            for bit in bits:
                if bit is None:
                    continue
                key = sig.resolve(bit)
                drv = driver.get(key)
                if drv is not None and key not in sig.named and drv != idx and not infos[drv].seq:
                    found.append(drv)
        deps.append(found)
    return post_order(deps)


def post_order(deps: list[list[int]]) -> list[int]:
    seen = [False] * len(deps)
    result: list[int] = []
    for root in range(len(deps)):
        if seen[root]:
            continue
        seen[root] = True
        stack = [(root, 0)]
        while stack:
            node, cursor = stack.pop()
            while cursor < len(deps[node]) and seen[deps[node][cursor]]:
                cursor += 1
            if cursor < len(deps[node]):
                nxt = deps[node][cursor]
                seen[nxt] = True
                stack.append((node, cursor + 1))
                stack.append((nxt, 0))
            else:
                result.append(node)
    return result


def reorder_module(lines: list[str], modules: dict[str, Module],
                   table: dict[str, CellType]) -> list[str]:
    items = split_items(lines)
    slots = [i for i, item in enumerate(items) if item.cell]
    if len(slots) < 2:
        return lines
    name = _body(lines[0]).split()[1]
    sig = Signals(modules[name].widths, modules[name].layouts)
    for item in items:
        text = _body(item.lines[0])
        if not item.cell and text.startswith("  connect "):
            sig.add_connect(*_split_connect(text))
    sig.mark_named()
    blocks = [items[i].lines for i in slots]
    order = order_cells(blocks, sig, table, modules)
    out = list(items)
    for slot, idx in zip(slots, order, strict=True):
        out[slot] = items[slots[idx]]
    result = [line for item in out for line in item.lines]
    if sorted(result) != sorted(lines):
        raise OrderError("internal error: the reordered module is not a permutation of its lines")
    return result


def _operand_end(tokens: list[str], start: int) -> int:
    """The index after the signal operand that begins at ``start``."""
    if tokens[start] == "{":
        depth = 0
        for i in range(start, len(tokens)):
            depth += (tokens[i] == "{") - (tokens[i] == "}")
            if depth == 0:
                return i + 1
        raise OrderError("unclosed '{'")
    end = start + 1
    return end + 1 if end < len(tokens) and tokens[end].startswith("[") else end


def _split_connect(text: str) -> tuple[str, str]:
    """``  connect LHS RHS``: the two signal texts."""
    tokens = _TOKEN.findall(text.strip()[len("connect "):])
    if not tokens:
        raise OrderError(f"connect not read: {text!r}")
    cut = _operand_end(tokens, 0)
    if cut >= len(tokens):
        raise OrderError(f"connect not read: {text!r}")
    return " ".join(tokens[:cut]), " ".join(tokens[cut:])


def make_names_unique(lines: list[str], seen: set[str], ordinal: int) -> list[str]:
    """The module's lines with each cell name that an earlier module has renamed ``name$m<n>``;
    ``seen`` gains the names of this module."""
    out: list[str] = []
    mine: set[str] = set()
    for line in lines:
        body = _body(line)
        if body.startswith("  cell "):
            words = body.split()
            name = words[2]
            if name in seen:
                new = f"{name}$m{ordinal}"
                while new in seen or new in mine:
                    new += "_"
                ending = line[len(body):]
                line = f"  cell {words[1]} {new}{ending}"
                name = new
            mine.add(name)
        out.append(line)
    seen.update(mine)
    return out


def reorder_text(text: str, table: dict[str, CellType] | None = None) -> str:
    """The RTLIL text with each module's cells in operand-first order; all else byte for byte."""
    table = table if table is not None else load_table()
    lines = text.splitlines(keepends=True)
    modules = parse_modules(lines)
    out: list[str] = []
    module: list[str] = []
    seen_names: set[str] = set()
    ordinal = 0
    for line in lines:
        if module or _body(line).startswith("module "):
            module.append(line)
            if _body(line) == "end":
                ordinal += 1
                done = reorder_module(module[:-1], modules, table)
                out.extend(make_names_unique(done, seen_names, ordinal))
                out.append(line)
                module = []
        else:
            out.append(line)
    out.extend(module)
    return "".join(out)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: il_order.py IN.il OUT.il", file=sys.stderr)
        return 2
    try:
        with open(argv[1], encoding="utf-8", newline="") as src:
            text = src.read()
        with open(argv[2], "w", encoding="utf-8", newline="") as dst:
            dst.write(reorder_text(text))
    except OrderError as exc:
        print(f"il_order: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
