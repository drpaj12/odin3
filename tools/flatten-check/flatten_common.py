"""Shared pieces of flatten-diff and flatten-sim (3A spec 14.3, 14.4): running Odin III and Yosys
under a memory cap and reading the RTLIL both write."""

from __future__ import annotations

import re
import subprocess
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

import memcap

CAP_BYTES = 2 << 30
EXIT_SAME, EXIT_DIFF, EXIT_ERROR, EXIT_SKIP = 0, 1, 2, 77


@dataclass
class RtlilModule:
    """What flatten-diff compares of one module: wire widths and cells."""

    wires: dict[str, int] = field(default_factory=dict)
    cells: list[tuple[str, str]] = field(default_factory=list)  # (type, name)


def run_capped(argv: Sequence[str]) -> subprocess.CompletedProcess[str]:
    """argv under the memory cap; memcap.CapUnavailable propagates (the caller exits 77)."""
    return memcap.run(list(argv), CAP_BYTES, capture_output=True)


_WIRE_WIDTH = re.compile(r"^\s+wire\s+(?:width\s+(\d+)\s+)?(?:.*\s)?(\S+)\s*$")


def parse_rtlil(text: str) -> dict[str, RtlilModule]:
    """The modules of an RTLIL text: public and private wires with widths, cells with types.

    Names lose their leading backslash; private names keep their dollar sign.
    """
    modules: dict[str, RtlilModule] = {}
    current: RtlilModule | None = None
    for line in text.splitlines():
        words = line.split()
        if not words:
            continue
        if words[0] == "module":
            current = modules.setdefault(words[1].lstrip("\\"), RtlilModule())
        elif current is not None and words[0] == "wire":
            width = 1
            if "width" in words:
                width = int(words[words.index("width") + 1])
            current.wires[words[-1].lstrip("\\")] = width
        elif current is not None and words[0] == "cell" and len(words) >= 3:
            current.cells.append((words[1].lstrip("\\"), words[2].lstrip("\\")))
    return modules


def cell_types(module: RtlilModule) -> Counter[str]:
    """The multiset of cell types."""
    return Counter(kind for kind, _ in module.cells)


def read_text(path: Path) -> str:
    return path.read_text(errors="replace")
