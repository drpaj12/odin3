#!/usr/bin/env python3
"""Step 6: name survival + name-free join (extracted cell -> DEF placement by master + location -> provenance).
Usage: step6_join.py PDK_SKY130A OUTDIR TOP  (reads magic_hier/TOP.ext, or/route.def, prov.csv; writes join.json, join.csv)"""
import csv
import json
import re
import sys
from pathlib import Path

pdk, out, top = sys.argv[1:4]
out = Path(out)
lef = Path(f"{pdk}/libs.ref/sky130_fd_sc_hd/lef/sky130_fd_sc_hd.lef").read_text()
size = {m.group(1): (float(m.group(2)), float(m.group(3)))
        for m in re.finditer(r"MACRO (\S+).*?SIZE ([\d.]+) BY ([\d.]+)", lef, re.S)}
# DEF components
comps = {}
for m in re.finditer(r"- (\S+) (\S+)(?: \+ SOURCE \w+)? \+ (?:FIXED|PLACED) \( (-?\d+) (-?\d+) \) (\w+)",
                     (out / "or/route.def").read_text()):
    comps[(m.group(2), int(m.group(3)), int(m.group(4)))] = m.group(1)
prov = {r["inst"]: r for r in csv.DictReader(open(out / "prov.csv"))}
# extracted uses (Magic units: 5 nm)
rows, n_logic, matched, ambiguous = [], 0, 0, 0
for m in re.finditer(r"^use (\S+) (\S+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)",
                     (out / f"magic_hier/{top}.ext").read_text(), re.M):
    master, mid = m.group(1), m.group(2)
    if not master.startswith("sky130_fd_sc_hd__"):
        continue
    a, b, c, d, e, f = (int(m.group(i)) for i in range(3, 9))
    w, h = (round(v * 1000) // 5 for v in size[master])
    xs = [a * x + b * y + c for x in (0, w) for y in (0, h)]
    ys = [d * x + e * y + f for x in (0, w) for y in (0, h)]
    key = (master, min(xs) * 5, min(ys) * 5)
    is_phys = re.search(r"__(tap|fill|decap)", master) is not None
    name = comps.get(key)
    p = prov.get(name) if name else None
    if not is_phys:
        n_logic += 1
        matched += name is not None
    rows.append(dict(extracted=mid, master=master, x=key[1], y=key[2], def_comp=name or "",
                     physical=int(is_phys), odin_provenance=int(p is not None and bool(p["src_chain"]))))
with open(out / "join.csv", "w", newline="") as fh:
    w_ = csv.DictWriter(fh, fieldnames=list(rows[0]))
    w_.writeheader()
    w_.writerows(rows)
logic = [r for r in rows if not r["physical"]]
# name survival: Odin instance names in GDS bytes and extracted spice
gds = (out / f"{top}.gds").read_bytes()
named = (out / f"{top}.named.gds").read_bytes()
spice = (out / "ext_hier.spice").read_text() + (out / "ext_flat.spice").read_text()
surv_gds = sum(1 for p in prov.values() if p["inst"].encode() in gds or p["odin_name"].encode() in gds)
surv_named = sum(1 for p in prov.values() if p["inst"].encode() in named)
surv_spice = sum(1 for p in prov.values() if re.search(rf"\b{re.escape(p['inst'])}\b", spice))
res = dict(extracted_cells=len(rows), extracted_logic_cells=len(logic), join_matched=matched,
           join_rate=round(matched / len(logic), 4) if logic else None,
           logic_with_hdl_chain=sum(r["odin_provenance"] for r in logic),
           hdl_chain_share_of_extracted_logic=round(sum(r["odin_provenance"] for r in logic) / len(logic), 4),
           odin_instances=len(prov), odin_names_in_gds=surv_gds, odin_names_in_gds_with_klayout_inst_name_properties=surv_named, odin_names_in_extracted_spice=surv_spice)
(out / "join.json").write_text(json.dumps(res, indent=1))
print(res)
