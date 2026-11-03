#!/usr/bin/env python3
"""DEF + LEF to GDS with the KLayout python module; std cell layouts come from the PDK GDS.

Usage: klayout_gds.py PDK_SKY130A OUTDIR TOP  (run with the Python that has ``klayout``).
Writes TOP.gds (instance and net names stripped) and TOP.named.gds (KLayout stores the DEF
instance names as GDS properties; kept as a measured variant of the name-survival question).
"""

from __future__ import annotations

import importlib
import sys
from typing import Any


def build(db: Any, pdk: str, out: str, top: str, names: bool, path: str) -> None:
    ref = f"{pdk}/libs.ref/sky130_fd_sc_hd"
    layout = db.Layout()
    opt = db.LoadLayoutOptions()
    cfg = db.LEFDEFReaderConfiguration()
    cfg.lef_files = [f"{ref}/techlef/sky130_fd_sc_hd__nom.tlef", f"{ref}/lef/sky130_fd_sc_hd.lef"]
    cfg.read_all_layers = True
    cfg.map_file = f"{pdk}/libs.tech/klayout/tech/sky130A.map"
    cfg.macro_resolution_mode = 1  # LEF abstracts, so cell names match the GDS cells merged below
    cfg.produce_via_geometry = True
    cfg.produce_pins = True
    cfg.produce_labels = True
    cfg.produce_inst_names = names
    cfg.produce_net_names = names
    opt.lefdef_config = cfg
    layout.read(f"{out}/or/route.def", opt)
    over = db.LoadLayoutOptions()
    over.cell_conflict_resolution = db.LoadLayoutOptions.CellConflictResolution.OverwriteCell
    layout.read(f"{ref}/gds/sky130_fd_sc_hd.gds", over)
    top_cell = layout.cell(top)
    for cell in list(layout.top_cells()):
        if cell.cell_index() != top_cell.cell_index():
            cell.prune_cell()
    if not names:  # KLayout stores DEF names as properties whatever the option says: strip them
        for cell in layout.each_cell():
            for inst in list(cell.each_inst()):
                inst.prop_id = 0
            for li in layout.layer_indexes():
                for shape in list(cell.shapes(li).each()):
                    if shape.prop_id:
                        shape.prop_id = 0
    layout.write(path)
    print(path.rsplit("/", 1)[-1], "cells", layout.cells(), "top", top_cell.name)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: klayout_gds.py PDK_SKY130A OUTDIR TOP", file=sys.stderr)
        return 2
    pdk, out, top = argv
    db = importlib.import_module("klayout.db")
    build(db, pdk, out, top, False, f"{out}/{top}.gds")
    build(db, pdk, out, top, True, f"{out}/{top}.named.gds")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
