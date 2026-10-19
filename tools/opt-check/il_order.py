"""il_order: the cells of an RTLIL file in operand-first order (3G spec WR-7, amended 2026-10-19).

Yosys's ``wreduce`` visits cells in the order their names were interned, which for its own front
end is operand before consumer; Odin III's ``write_rtlil`` writes node-ID order, and the elaborator
numbers a consumer first. ``opt-diff`` and ``opt-rules`` hand Yosys an Odin III RTLIL file, so
they put its cells in the order Yosys's front end would have made them: a depth-first post-order
over the cells, entered in file order, the driver of each input port (port order in the file)
placed before its reader. A flip-flop, latch or memory output is not followed (as in
``src/passes/opt/wreduce_order.c``, which this mirrors), a loop is cut where the walk meets it.
Only the order of the ``cell ... end`` blocks of a module changes; every other line stays.

Usage: ``il_order.py IN.il OUT.il``.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SOURCE_TYPES = frozenset({"$dff", "$dffe", "$adff", "$adffe", "$sdff", "$sdffe", "$sdffce",
                          "$dlatch", "$adlatch", "$mem"})
OUTPUT_PORTS = frozenset({"Y", "Q", "RD_DATA", "O"})
_NAME = re.compile(r"[\\$][^\s{}\[\]]+")

Cell = tuple[list[str], str, list[tuple[str, str]]]  # lines, type, (port, signal text) per connect


def parse_cell(lines: list[str]) -> Cell:
    """The block's type and its ``connect \\PORT signal`` pairs."""
    start = next(i for i, line in enumerate(lines) if line.startswith("  cell "))
    ctype = lines[start].split()[1]
    conns = []
    for line in lines[start + 1:]:
        words = line.split(None, 2)
        if len(words) == 3 and words[0] == "connect":
            conns.append((words[1].lstrip("\\"), words[2]))
    return lines, ctype, conns


def split_module(lines: list[str]) -> tuple[list[str], list[list[str]], list[str]]:
    """(lines before the first cell, the cell blocks, lines after the last cell). Cells are
    expected together; any other line between two of them stays after the cells."""
    head: list[str] = []
    cells: list[list[str]] = []
    tail: list[str] = []
    block: list[str] | None = None
    pending: list[str] = []  # the attribute lines that belong to the next cell
    for line in lines:
        if block is not None:
            block.append(line)
            if line.strip() == "end":
                cells.append(block)
                block = None
        elif line.startswith("  attribute "):
            pending.append(line)
        elif line.startswith("  cell "):
            block = [*pending, line]
            pending = []
        else:
            (tail if cells else head).extend([*pending, line])
            pending = []
    (tail if cells else head).extend(pending)
    if block is not None:
        raise ValueError("cell without end")
    return head, cells, tail


def alias_map(lines: list[str]) -> dict[str, list[str]]:
    """``connect LHS RHS`` of the module: the names of the right-hand side, per left-hand name."""
    found: dict[str, list[str]] = {}
    for line in lines:
        words = line.split(None, 2)
        if len(words) == 3 and words[0] == "connect" and len(words[1].split()) == 1:
            names = _NAME.findall(words[1])
            if len(names) == 1:
                found.setdefault(names[0], []).extend(_NAME.findall(words[2]))
    return found


def leaves(signal: str, alias: dict[str, list[str]]) -> list[str]:
    """The names a signal text stands for, following module-level connects (iterative)."""
    out: list[str] = []
    seen: set[str] = set()
    stack = _NAME.findall(signal)[::-1]
    while stack:
        name = stack.pop()
        if name in seen:
            continue
        seen.add(name)
        if name in alias:
            stack.extend(alias[name][::-1])
        else:
            out.append(name)
    return out


def order_cells(cells: list[list[str]], alias: dict[str, list[str]]) -> list[list[str]]:
    """The cell blocks in operand-first post-order."""
    parsed = [parse_cell(c) for c in cells]
    driver: dict[str, int] = {}
    for idx, (_, ctype, conns) in enumerate(parsed):
        if ctype in SOURCE_TYPES:
            continue
        for port, sig in conns:
            if port in OUTPUT_PORTS:
                for name in leaves(sig, alias):
                    driver.setdefault(name, idx)
    deps: list[list[int]] = []
    for idx, (_, _, conns) in enumerate(parsed):
        found: list[int] = []
        for port, sig in conns:
            if port not in OUTPUT_PORTS:
                found.extend(driver[n] for n in leaves(sig, alias) if n in driver)
        deps.append([d for d in found if d != idx])
    seen = [False] * len(parsed)
    result: list[int] = []
    for root in range(len(parsed)):
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
    return [parsed[i][0] for i in result]


def reorder_text(text: str) -> str:
    """The RTLIL text with each module's cells in operand-first order."""
    out: list[str] = []
    module: list[str] = []
    for line in text.splitlines():
        module.append(line)
        if line == "end":  # the module's end (cell ends are indented)
            out.extend(reorder_module(module))
            module = []
    out.extend(module)
    return "\n".join(out) + "\n"


def reorder_module(lines: list[str]) -> list[str]:
    head, cells, tail = split_module(lines)
    if len(cells) < 2:
        return lines
    alias = alias_map(lines)
    return head + [line for cell in order_cells(cells, alias) for line in cell] + tail


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: il_order.py IN.il OUT.il", file=sys.stderr)
        return 2
    Path(argv[2]).write_text(reorder_text(Path(argv[1]).read_text()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
