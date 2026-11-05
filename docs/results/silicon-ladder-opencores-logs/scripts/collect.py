#!/usr/bin/env python3
"""collect.py NAME CLOCK: one JSON row per design from the run directory."""
import json, re, sys
from pathlib import Path
OUT = Path("~/odin3-ws/work/silicon-ladder-out").expanduser()
n, clk = sys.argv[1], sys.argv[2]
d = OUT / n
def rd(p):
    try: return Path(p).read_text(errors="replace")
    except OSError: return ""
row = {"design": n, "clock_port": clk}
flow = rd(d/"flow.out")
steps = re.findall(r"^(\w+): (ok|FAILED.*|OUT OF SCOPE.*)$", flow, re.M)
row["steps_ok"] = [s for s, r in steps if r == "ok"]
row["steps_reached"] = len(row["steps_ok"])
bad = [f"{s}: {r}" for s, r in steps if r != "ok"]
row["stop"] = bad[0] if bad else ""
ft = rd(d/"flow.time")
m = re.search(r"maxrss_kb=(\d+) wall=([\d.]+)", ft); row["peak_rss_mb"] = round(int(m[1])/1024, 1) if m else ""
row["wall_s"] = float(m[2]) if m else ""
m = re.search(r"flow rc=(\d+)", ft); row["flow_rc"] = int(m[1]) if m else ""
if flow and not steps and row.get("flow_rc") == 137:
    pass
if row.get("flow_rc") == 137 and not steps:
    row["stop"] = "map: process killed (SIGKILL, signal 9) at the 6 GiB heavy-run memory cap, peak RSS %s MB; Python gates_objects (silicon_flow.py:583) during the map step provenance build" % row["peak_rss_mb"]
elif not steps:
    row["stop"] = row["stop"] or ("no step lines; flow.err: " + rd(d/"flow.err").strip()[-300:].replace("\n", " | "))
meas = dict(re.findall(r"MEASURE (\w+)=(\S+)", rd(d/"measure.log")))
for k, v in meas.items():
    row[k] = v
try:
    core, la = float(meas["core_um2"]), float(meas["logic_area_um2"]); ia = float(meas["inst_area_all_um2"])
    row["util_logic_pct"] = round(100*la/core, 1)
except (KeyError, ValueError): pass
try:
    c = json.loads(rd(d/"flow/census.json")); row["_census_steps"] = len(c)
except json.JSONDecodeError: pass
try:
    j = json.loads(rd(d/"flow/join.json"))
    row.update({"odin_cells": j["odin_instances"], "logic_cells_final": j["logic_cells"], "join_matched": j["join_matched"],
                "join_rate": j["join_rate"], "logic_with_hdl_chain": j["logic_with_hdl_chain"],
                "extracted_cells": j["extracted_cells"], "names_in_gds": j["names_in_gds"],
                "names_in_extracted_spice": j["names_in_extracted_spice"]})
except (json.JSONDecodeError, KeyError): pass
try:
    cs = json.loads(rd(d/"flow/census.json"))
    kinds = {}
    for s in cs:
        for k, v in s["inserted_kinds"].items(): kinds[k] = kinds.get(k, 0) + v
    row["inserted_by_kind"] = kinds
    row["resized"] = sum(s["resized"] for s in cs); row["removed"] = sum(s["removed"] for s in cs); row["renamed"] = sum(s["renamed"] for s in cs)
except (json.JSONDecodeError, KeyError): pass
if "odin_cells" not in row and (d/"flow/prov.csv").exists():
    row["odin_cells"] = max(0, len(rd(d/"flow/prov.csv").splitlines()) - 1)
lvs = rd(d/"flow/lvs.log")
fr = re.findall(r"^Final result:.*$", lvs, re.M)
row["lvs"] = fr[-1] if fr else ("not run" if not lvs else "no final-result line")
drc = rd(d/"flow/or/drc.rpt")
row["drc_violations"] = len(re.findall(r"^violation type", drc, re.M)) if (d/"flow/or/drc.rpt").exists() else "not run"
try:
    e = json.loads(rd(d/"flow/prov/04-equiv.json"))["check"]; row["equiv"] = e["result"]; row["equiv_detail"] = e["detail"][:200]
except (json.JSONDecodeError, KeyError): row["equiv"] = "not run"
for k in ("rtl", "gate"):
    t = rd(d/f"sim_{k}.txt")
    m = re.search(r"VERDICT (.*)", t)
    if m and "iverilog rc=0" not in t:
        iv = rd(d/f"sim_{k}/iv.log").strip().splitlines()
        row[f"sim_{k}"] = "not run: iverilog error: " + (iv[0] if iv else "?")[:200]; continue
    if m and m[1].startswith("fail-or-not-run"):
        m = re.search(r"bad=(\d+)", m[1]); row[f"sim_{k}"] = "FAIL (%s FAIL/ERROR lines in sim.log)" % m[1]; continue
    row[f"sim_{k}"] = m[1] if m else ("not run: " + t.strip()[:150].replace("\n", " | ") if t else "not run (no final.v)")
print(json.dumps(row))
(d/"row.json").write_text(json.dumps(row, indent=1))
