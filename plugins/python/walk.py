"""Walk a design through the Python binding: per-module counts and a cell-type histogram.

The lines are worded as the C `stats` pass words its log messages (without the "stats: "
prefix), so the two can be compared line for line; --check does that for each file.

Run: python3 plugins/python/walk.py [--techlib LIB]... [--check] FILE.blif...
"""

from __future__ import annotations

import argparse
import sys
from collections import Counter
from collections.abc import Sequence

from odin3 import Design

STATS_PREFIX = "stats: "


def _c_order(name: str) -> bytes:
    """The byte order the C pass sorts type names in (strcmp)."""
    return name.encode("utf-8", "surrogateescape")


def walk_lines(design: Design) -> list[str]:
    """Walk every module from Python: its ports, live nodes, nets and wires, and cell types."""
    modules = list(design.modules())
    top = design.top()
    lines = [f"design: modules {len(modules)}, top {top.name if top else '(none)'}"]
    for module in modules:
        name = module.name
        nodes = list(module.nodes())
        nets = sum(1 for _ in module.nets())
        wires = sum(1 for _ in module.wires())
        ports = len(module.ports())
        lines.append(
            f"module {name}: ports {ports}, nodes {len(nodes)}, nets {nets}, wires {wires}"
        )
        histogram = Counter(node.type_name for node in nodes)
        for cell in sorted(histogram, key=_c_order):
            lines.append(f"module {name}: cell {cell} {histogram[cell]}")
    return lines


def stats_lines(design: Design) -> list[str]:
    """Run the C `stats` pass on the design and return its messages, prefix removed."""
    with design.odin3.capture_log() as log:
        design.run_pass("stats")
    return [rec.message[len(STATS_PREFIX) :] for rec in log if rec.message.startswith(STATS_PREFIX)]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else None)
    parser.add_argument("--techlib", action="append", default=[], help="read this .o3lib first")
    parser.add_argument("--check", action="store_true", help="compare with the stats pass")
    parser.add_argument("files", nargs="+", help="BLIF netlists")
    args = parser.parse_args(argv)
    failed = 0
    for path in args.files:
        with Design.read_blif(path, techlibs=args.techlib) as design:
            lines = walk_lines(design)
            print(f"{path}:")
            for line in lines:
                print(f"  {line}")
            if args.check:
                same = lines == stats_lines(design)
                failed += 0 if same else 1
                print("  walk == stats" if same else "  walk != stats", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
