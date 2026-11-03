#!/usr/bin/env python3
"""Census of instance changes between consecutive OpenROAD step dumps + provenance share.
Usage: census.py OUTDIR  (reads prov.csv, or/s*.csv; writes census.csv, census.json)"""
import csv
import glob
import json
import os
import sys

out = sys.argv[1]
prov = {r["inst"]: r for r in csv.DictReader(open(f"{out}/prov.csv"))}
steps = sorted(glob.glob(f"{out}/or/s*.csv"))
steps = [s for s in steps if os.path.basename(s)[0] == "s" and os.path.basename(s)[1:3].isdigit()]


def load(p):
    return {r["inst"]: r for r in csv.DictReader(open(p))}


def kind(master, step):
    m = master.split("__", 1)[-1]
    if m.startswith("tap"):
        return "tap"
    if m.startswith(("fill", "decap")):
        return "fill"
    if m.startswith("diode"):
        return "antenna"
    if step == "s07_cts":
        return "cts_buffer"
    if step == "s08_repair_timing":
        return "hold_or_setup_buffer"
    if m.startswith(("buf", "clkbuf", "inv")):
        return "repair_buffer"
    return "other_cell"


rows, prev = [], None
for p in steps:
    name = os.path.basename(p)[:-4]
    cur = load(p)
    c = dict(step=name, total=len(cur), kept=0, moved=0, resized=0, inserted=0, removed=0, renamed=0)
    ins_kinds = {}
    if prev is not None:
        rem = {n: r for n, r in prev.items() if n not in cur}
        add = {n: r for n, r in cur.items() if n not in prev}
        for n, r in cur.items():
            if n in prev:
                if prev[n]["master"] != r["master"]:
                    c["resized"] += 1
                elif (prev[n]["x"], prev[n]["y"]) != (r["x"], r["y"]):
                    c["moved"] += 1
                else:
                    c["kept"] += 1
        # renamed: same master, same place, other name
        rkey = {(r["master"], r["x"], r["y"]): n for n, r in rem.items()}
        for n, r in list(add.items()):
            k = (r["master"], r["x"], r["y"])
            if k in rkey:
                c["renamed"] += 1
                del rem[rkey[k]]
                del add[n]
        c["removed"] = len(rem)
        c["inserted"] = len(add)
        for n, r in add.items():
            k = kind(r["master"], name)
            ins_kinds[k] = ins_kinds.get(k, 0) + 1
    else:
        c["kept"] = len(cur)
    c["inserted_kinds"] = json.dumps(ins_kinds, sort_keys=True)
    rows.append(c)
    prev = cur
with open(f"{out}/census.csv", "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0]))
    w.writeheader()
    w.writerows(rows)
final = prev
logic = [n for n, r in final.items() if kind(r["master"], "") == "other_cell" or n in prov]
with_chain = [n for n in logic if n in prov and prov[n]["src_chain"]]
phys = [n for n in final if n not in prov]
kinds = {}
for n in phys:
    k = kind(final[n]["master"], "x")
    kinds[k] = kinds.get(k, 0) + 1
summ = dict(final_instances=len(final), odin_cells=len(prov),
            odin_cells_surviving_by_name=sum(1 for n in prov if n in final),
            odin_cells_resized=sum(1 for n in prov if n in final and final[n]["master"] != prov[n]["master"]),
            logic_cells=len(logic), logic_with_hdl_chain=len(with_chain),
            hdl_chain_share=round(len(with_chain) / len(logic), 4) if logic else None,
            physical_origin_cells=len(phys), physical_origin_by_kind=kinds)
json.dump(summ, open(f"{out}/census.json", "w"), indent=1)
for r in rows:
    print({k: v for k, v in r.items()})
print(summ)
