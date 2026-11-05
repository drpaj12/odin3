#!/usr/bin/env python3
import json
from pathlib import Path
OUT = Path("~/odin3-ws/work/silicon-ladder-out").expanduser()
dest = Path("~/odin3-ws/work/odin3-silicon-ladder/docs/results/silicon-ladder-opencores.tsv").expanduser()
order = ["mmuart", "simple_spi", "i2c", "wb_lcd", "verilog_cordic_core", "des", "sha3_low"]
cols = ["design", "clock_port", "steps_reached", "stop", "odin_cells", "logic_cells_final", "inst_count_all", "core_um2", "die_um2",
        "logic_area_um2", "util_logic_pct", "wns_ns", "tns_ns", "hold_ws_ns", "drc_violations", "lvs", "equiv", "equiv_detail",
        "join_matched", "join_rate", "logic_with_hdl_chain", "names_in_gds", "names_in_extracted_spice", "resized", "removed", "renamed",
        "inserted_by_kind", "sim_rtl", "sim_gate", "wall_s", "peak_rss_mb", "flow_rc"]
rows = []
for n in order:
    p = OUT / n / "row.json"
    if p.exists(): rows.append(json.loads(p.read_text()))
dest.parent.mkdir(parents=True, exist_ok=True)
with dest.open("w") as f:
    f.write("\t".join(cols) + "\n")
    for r in rows:
        f.write("\t".join(str(r.get(c, "")).replace("\t", " ").replace("\n", " ") for c in cols) + "\n")
print(len(rows), "rows ->", dest)
