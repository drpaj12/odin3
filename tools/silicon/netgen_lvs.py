#!/usr/bin/env python3
"""Step 5: Netgen LVS of Magic-extracted SPICE (hier) against the OpenROAD final netlist (Odin cells + inserted).
Usage: step5_lvs.py TOOLS PDK_SKY130A OUTDIR TOP"""
import re
import subprocess
import sys
from pathlib import Path

tools, pdk, out, top = sys.argv[1:5]
out = Path(out)
cells = Path(f"{pdk}/libs.ref/sky130_fd_sc_hd/spice/sky130_fd_sc_hd.spice").read_text()
pins = {m.group(1): m.group(2).split() for m in re.finditer(r"^\.subckt (\S+) ([^\n]*(?:\n\+[^\n]*)*)", cells, re.M)}
pins = {k: v.replace("\n+", " ").split() if isinstance(v, str) else v for k, v in pins.items()}
ext = (out / "ext_hier.spice").read_text()
m = re.search(rf"^\.subckt {re.escape(top)} ([^\n]*(?:\n\+[^\n]*)*)", ext, re.M)
ports = m.group(1).replace("\n+", " ").split()
v = re.sub(r"//[^\n]*", "", (out / "or" / "final.v").read_text())
pg = {"VPWR": "VDD", "VPB": "VDD", "VGND": "VSS", "VNB": "VSS"}
lines = [f".subckt {top} {' '.join(ports)}"]
for im in re.finditer(r"^\s*(sky130_fd_sc_hd__\w+)\s+(\S+)\s*\((.*?)\);", v, re.M | re.S):
    master, inst, body = im.groups()
    if re.search(r"__(tap|fill|decap)", master):
        continue
    conn = dict(re.findall(r"\.(\w+)\(([^)]*)\)", body))
    conn = {k: x.strip() for k, x in conn.items()}
    for p, n in pg.items():
        conn.setdefault(p, n)
    lines.append(f"X{inst} " + " ".join(conn[p] for p in pins[master]) + f" {master}")
lines.append(f".ends {top}")
(out / "ref.spice").write_text(cells + "\n" + "\n".join(lines) + "\n")
setup = f"{pdk}/libs.tech/netgen/sky130A_setup.tcl"
(out / "lvs.tcl").write_text(f'lvs "{out}/ext_hier.spice {top}" "{out}/ref.spice {top}" {setup} {out}/lvs.log\nquit\n')
r = subprocess.run([f"{tools}/mamba/envs/eda/bin/netgen", "-batch", "source", str(out / "lvs.tcl")],
                   capture_output=True, text=True)
(out / "netgen.out").write_text(r.stdout + r.stderr)
log = (out / "lvs.log").read_text() if (out / "lvs.log").exists() else ""
res = [l for l in log.splitlines() if "uivalent" in l or "do not match" in l or "match uniquely" in l]
print("\n".join(res[-4:]) or r.stdout[-400:])
