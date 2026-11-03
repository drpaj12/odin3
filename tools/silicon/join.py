#!/usr/bin/env python3
"""Name-free join of Magic-extracted cells to DEF placements by cell type and location.

Usage: join.py PDK_SKY130A OUTDIR TOP  (reads magic_hier/TOP.ext, or/route.def, prov.csv)
"""

from __future__ import annotations

import csv
import sys
from pathlib import Path

import silicon_flow


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: join.py PDK_SKY130A OUTDIR TOP", file=sys.stderr)
        return 2
    pdk, out, top = Path(argv[0]), Path(argv[1]), argv[2]
    sizes = silicon_flow.lef_sizes(pdk / "libs.ref/sky130_fd_sc_hd/lef/sky130_fd_sc_hd.lef")
    comps = silicon_flow.def_components(out / "or" / "route.def")
    cells = silicon_flow.extracted_cells(out / "magic_hier" / f"{top}.ext", sizes)
    with (out / "prov.csv").open() as fh:
        prov = {r["inst"] for r in csv.DictReader(fh)}
    logic = [c for c in cells if silicon_flow.PHYS_RE.search(c["master"]) is None]
    joined = [c for c in logic if (c["master"], c["x"], c["y"]) in comps]
    in_prov = [c for c in joined if comps[(c["master"], c["x"], c["y"])] in prov]
    print(f"logic cells {len(logic)}, joined {len(joined)}, with Odin provenance {len(in_prov)}")
    return 0 if len(joined) == len(logic) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
