#!/usr/bin/env python3
"""Step 3: DEF + LEF -> GDS with KLayout (python module), std-cell layouts merged from the PDK GDS.
Writes TOP.gds (instance names NOT stored) and TOP.named.gds (KLayout stores DEF instance names as GDS
properties). Usage: step3_gds.py PDK_SKY130A OUTDIR TOP"""
import sys

import klayout.db as db

pdk, out, top = sys.argv[1:4]
ref = f"{pdk}/libs.ref/sky130_fd_sc_hd"


def build(names, path):
    lyt = db.Layout()
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
    lyt.read(f"{out}/or/route.def", opt)
    o2 = db.LoadLayoutOptions()
    o2.cell_conflict_resolution = db.LoadLayoutOptions.CellConflictResolution.OverwriteCell
    lyt.read(f"{ref}/gds/sky130_fd_sc_hd.gds", o2)
    topc = lyt.cell(top)
    for c in list(lyt.top_cells()):
        if c.cell_index() != topc.cell_index():
            c.prune_cell()
    if not names:  # KLayout stores DEF instance/net names as GDS properties whatever the option says: strip them
        for ci in lyt.each_cell():
            for inst in list(ci.each_inst()):
                inst.prop_id = 0
            for li in lyt.layer_indexes():
                for sh in list(ci.shapes(li).each()):
                    if sh.prop_id:
                        sh.prop_id = 0
    lyt.write(path)
    print(path.split("/")[-1], "cells", lyt.cells(), "top", topc.name)


build(False, f"{out}/{top}.gds")
build(True, f"{out}/{top}.named.gds")
