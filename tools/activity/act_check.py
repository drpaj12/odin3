#!/usr/bin/env python3
"""act_check: compare the activity pass with ACE 2.0, with simulation, and through VPR power.

Spec docs/specs/2026-11-08-activity-design.md section 8. Reads run directories laid out like the
``wyrd power run`` output (``DIR/fpga/vectorless/{circuit.blif,in.blif,ace.act}`` from ACE 2.0 and,
when a testbench ran, ``DIR/fpga/tb/circuit.blif`` and ``DIR/fpga/tb.act`` from simulation), runs
``odin3 activity`` on the same BLIFs and prints one JSON object per design plus a markdown table.

  act_check.py --odin3 BIN --vtr ROOT [--ace-time] [--power] [--out DIR] DIR...

Per design: nets common to both files (clock excluded), Pearson and Spearman correlation, mean
absolute error, mean signed error (ours minus reference) and the fraction within 0.05, for P1 and
density; run time and peak RSS of the pass and (--ace-time) of ACE 2.0; (--power) VPR --power total,
dynamic and clock power with each activity file on one placement. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

Act = dict[str, tuple[float, float]]
ACE_LIKE = "--opaque-type adder --ff-start 0"


def read_act(path: Path) -> Act:
    out: Act = {}
    for line in path.read_text().splitlines():
        words = line.split()
        if len(words) == 3:
            out[words[0]] = (float(words[1]), float(words[2]))
    return out


def pearson(xs: list[float], ys: list[float]) -> float:
    n = len(xs)
    if n < 2:
        return float("nan")
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx == 0 or syy == 0:
        return float("nan")
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys, strict=True)) / math.sqrt(sxx * syy)


def ranks(vals: list[float]) -> list[float]:
    order = sorted(range(len(vals)), key=vals.__getitem__)
    out = [0.0] * len(vals)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and vals[order[j + 1]] == vals[order[i]]:
            j += 1
        for k in range(i, j + 1):
            out[order[k]] = (i + j) / 2
        i = j + 1
    return out


def stats(ours: list[float], ref: list[float]) -> dict[str, float]:
    n = len(ours)
    diff = [a - b for a, b in zip(ours, ref, strict=True)]
    return {
        "pearson": pearson(ours, ref),
        "spearman": pearson(ranks(ours), ranks(ref)),
        "mae": sum(abs(d) for d in diff) / n if n else float("nan"),
        "bias": sum(diff) / n if n else float("nan"),
        "within_0.05": sum(1 for d in diff if abs(d) <= 0.05) / n if n else float("nan"),
    }


def compare(ours: Act, ref: Act, exclude: set[str]) -> dict[str, Any]:
    names = sorted(n for n in ours if n in ref and n not in exclude)
    return {
        "nets": len(names),
        "p1": stats([ours[n][0] for n in names], [ref[n][0] for n in names]),
        "density": stats([ours[n][1] for n in names], [ref[n][1] for n in names]),
    }


def timed(argv: list[str], cwd: Path) -> tuple[subprocess.CompletedProcess[str], float, int]:
    """Runs argv under /usr/bin/time -v: (result, wall seconds, peak RSS in KiB)."""
    res = subprocess.run(["/usr/bin/time", "-v", *argv], cwd=cwd, capture_output=True, text=True)
    wall = 0.0
    rss = 0
    for line in res.stderr.splitlines():
        if "Elapsed (wall clock)" in line:
            parts = line.rsplit(" ", 1)[1].split(":")
            wall = sum(float(p) * 60**i for i, p in enumerate(reversed(parts)))
        elif "Maximum resident set size" in line:
            rss = int(line.rsplit(" ", 1)[1])
    return res, wall, rss


def blif_clock(blif: Path) -> str:
    for line in blif.read_text().splitlines():
        words = line.split()
        if len(words) >= 6 and words[0] == ".latch":
            return words[4]
    return ""


def run_activity(odin3: Path, lib: Path, blif: Path, out: Path, extra: str) -> dict[str, Any]:
    script = f"read_techlib {lib}; read_blif {blif}; activity {extra} --out {out} --json {out}.json"
    res, wall, rss = timed([str(odin3), "-p", script], out.parent)
    if res.returncode != 0:
        raise RuntimeError(f"odin3 activity failed on {blif}: {res.stderr[-400:]}")
    info = json.loads(Path(f"{out}.json").read_text())
    return {
        "wall_s": wall,
        "rss_kib": rss,
        "pass_ms": info["ms"],
        "iterations": info["iterations"],
        "converged": info["converged"],
        "opaque_cells": info["opaque_cells"],
    }


def vpr_power(vtr: Path, blif: Path, act: Path, work: Path) -> dict[str, float]:
    work.mkdir(parents=True, exist_ok=True)
    shutil.copy(blif, work / "circuit.blif")
    arch = vtr / "vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml"
    tech = vtr / "vtr_flow/tech/PTM_45nm/45nm.xml"
    argv = [
        str(vtr / "build/vpr/vpr"),
        str(arch),
        "circuit.blif",
        "--route_chan_width",
        "100",
        "--power",
        "--tech_properties",
        str(tech),
        "--activity_file",
        str(act),
    ]
    res = subprocess.run(argv, cwd=work, capture_output=True, text=True)
    power = sorted(work.glob("*.power"))
    if res.returncode != 0 or not power:
        raise RuntimeError(f"vpr failed in {work}")
    text = power[0].read_text()
    out: dict[str, float] = {}
    for key, label in (("total_w", "Total"), ("clock_w", " Clock"), ("routing_w", " Routing")):
        m = re.search(rf"^{label}\s+([0-9.eE+-]+)", text, re.M)
        if m:
            out[key] = float(m.group(1))
    return out


def safe_power(vtr: Path, blif: Path, act: Path, work: Path) -> dict[str, Any]:
    try:
        return vpr_power(vtr, blif, act, work)
    except (RuntimeError, OSError) as exc:
        return {"error": str(exc)}


def pi_lines(blif: Path, ref: Act) -> str:
    """The reference activity of the primary inputs, as a --pi-file body."""
    ins: list[str] = []
    for line in blif.read_text().splitlines():
        if line.startswith(".inputs"):
            ins += line.split()[1:]
    return "".join(f"{n} {ref[n][0]} {ref[n][1]}\n" for n in ins if n in ref)


def check_design(args: argparse.Namespace, design: Path) -> dict[str, Any]:
    work = Path(args.out).resolve() / design.name
    work.mkdir(parents=True, exist_ok=True)
    res: dict[str, Any] = {"design": design.name}
    vec = design / "fpga/vectorless"
    odin3, lib, vtr = Path(args.odin3).resolve(), Path(args.lib).resolve(), Path(args.vtr).resolve()
    clock = blif_clock(vec / "circuit.blif")
    if (vec / "ace.act").exists():
        # ACE 2.0 reads a hard `adder` as a box with primary-input outputs (--opaque-type adder)
        # and starts every flip-flop at 0 (--ff-start 0): "ACE-like" is both
        skip = {clock, "vcc", "gnd", "unconn"}
        ref = read_act(vec / "ace.act")
        ours = work / "ours_ace_like.act"
        res["run"] = run_activity(odin3, lib, vec / "circuit.blif", ours, ACE_LIKE)
        res["vs_ace"] = compare(read_act(ours), ref, skip)
        for key, flags in (
            ("vs_ace_start_half", "--opaque-type adder"),
            ("vs_ace_adders_propagated", "--ff-start 0"),
        ):
            other = work / f"ours_{key}.act"
            run_activity(odin3, lib, vec / "circuit.blif", other, flags)
            res[key] = compare(read_act(other), ref, skip)
        if args.mc_cycles > 0:
            mc_out = work / "mc.act"
            mc_cmd = [
                sys.executable,
                str(Path(__file__).with_name("act_mc.py")),
                str(vec / "circuit.blif"),
            ]
            subprocess.run(
                [*mc_cmd, "--cycles", str(args.mc_cycles), "--out", str(mc_out)], check=True
            )
            mc = read_act(mc_out)
            res["mc_ours"] = compare(read_act(work / "ours_vs_ace_adders_propagated.act"), mc, skip)
            res["mc_ace"] = compare(ref, mc, skip)
        if args.ace_time:
            ace = vtr / "build/ace2/ace"
            argv = [str(ace), "-b", str(vec / "in.blif"), "-c", clock, "-n", "x.blif"]
            _, wall, rss = timed([*argv, "-o", "x.act", "-s", "1"], work)
            res["ace"] = {"wall_s": wall, "rss_kib": rss}
        if args.power:
            res["power_ace"] = safe_power(
                vtr, vec / "circuit.blif", vec / "ace.act", work / "vpr_ace"
            )
            res["power_ours_ace_like"] = safe_power(
                vtr, vec / "circuit.blif", ours, work / "vpr_ours"
            )
    tb_act = design / "fpga/tb.act"
    tb_blif = design / "fpga/tb/circuit.blif"
    if tb_act.exists() and tb_blif.exists():
        ref = read_act(tb_act)
        pif = work / "pi.txt"
        pif.write_text(pi_lines(tb_blif, ref))
        pis = set(read_act(pif))
        skip = {clock, "vcc", "gnd", "unconn"} | pis
        variants = (
            ("same_pi", f"--pi-file {pif} --ff-start 0"),
            ("same_pi_start_half", f"--pi-file {pif}"),
            ("default_pi", "--ff-start 0"),
        )
        for name, flags in variants:
            out = work / f"ours_sim_{name}.act"
            run_activity(odin3, lib, tb_blif, out, flags)
            res[f"vs_sim_{name}"] = compare(read_act(out), ref, skip)
        if args.power:
            res["power_sim"] = safe_power(vtr, tb_blif, tb_act, work / "vpr_sim")
            res["power_ours_sim_pi"] = safe_power(
                vtr, tb_blif, work / "ours_sim_same_pi.act", work / "vpr_ours_pi"
            )
            res["power_ours_default_tb"] = safe_power(
                vtr, tb_blif, work / "ours_sim_default_pi.act", work / "vpr_ours_def"
            )
    return res


def table(results: list[dict[str, Any]]) -> str:
    rows = [
        "| design | reference | nets | P1 r | P1 MAE | D r | D MAE | D bias |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for r in results:
        for key, label in (
            ("vs_ace", "ACE 2.0 (ACE-like: adders opaque, flip-flops start 0)"),
            ("vs_ace_start_half", "ACE 2.0 (adders opaque, start 0.5)"),
            ("vs_ace_adders_propagated", "ACE 2.0 (adders propagated, start 0)"),
            ("vs_sim_same_pi", "sim, measured inputs, start 0"),
            ("vs_sim_same_pi_start_half", "sim, measured inputs, start 0.5"),
            ("vs_sim_default_pi", "sim, default inputs, start 0"),
            ("mc_ours", "Monte Carlo truth: activity (adders propagated)"),
            ("mc_ace", "Monte Carlo truth: ACE 2.0"),
        ):
            if key in r:
                c = r[key]
                rows.append(
                    f"| {r['design']} | {label} | {c['nets']} | {c['p1']['pearson']:.3f} | "
                    f"{c['p1']['mae']:.4f} | {c['density']['pearson']:.3f} | "
                    f"{c['density']['mae']:.4f} | {c['density']['bias']:+.4f} |"
                )
    return "\n".join(rows)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--odin3", required=True)
    ap.add_argument("--lib", required=True, help="lib/vtr.o3lib")
    ap.add_argument("--vtr", required=True, help="VTR root (build/ace2/ace, build/vpr/vpr)")
    ap.add_argument("--ace-time", action="store_true")
    ap.add_argument("--power", action="store_true")
    ap.add_argument(
        "--mc-cycles", type=int, default=0, help="also compare both with a Monte Carlo run"
    )
    ap.add_argument("--out", default=tempfile.mkdtemp(prefix="act-check-"))
    ap.add_argument("designs", nargs="+")
    args = ap.parse_args()
    results = []
    for d in args.designs:
        try:
            results.append(check_design(args, Path(d)))
        except (RuntimeError, OSError, KeyError) as exc:
            print(f"SKIP {d}: {exc}", file=sys.stderr)
    for r in results:
        print(json.dumps(r))
    print(table(results), file=sys.stderr)
    return 0 if results else 1


if __name__ == "__main__":
    sys.exit(main())
