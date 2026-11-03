#!/usr/bin/env python3
"""Netgen LVS of the Magic hierarchical extraction against the OpenROAD netlist.

Usage: netgen_lvs.py TOOLS_DIR OUTDIR TOP  (TOOLS_DIR as for silicon_flow.discover)
"""

from __future__ import annotations

import sys
from pathlib import Path

import silicon_flow


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: netgen_lvs.py TOOLS_DIR OUTDIR TOP", file=sys.stderr)
        return 2
    tools = silicon_flow.discover(Path("odin3"), Path(argv[0]), None)
    if tools.netgen is None or tools.pdk is None:
        print("netgen or the PDK not found", file=sys.stderr)
        return silicon_flow.SKIP_EXIT
    result = silicon_flow.run_lvs(tools, Path(argv[1]), argv[2])
    print(result["result"], result["detail"])
    return 0 if result["result"] == "match" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
