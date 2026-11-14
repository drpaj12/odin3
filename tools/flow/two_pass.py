"""two_pass: ``odin3-flow --two-pass`` (docs/user/timing-hints.md).

Pass 1 is the whole flow to routing, with a provenance depth that gives exact LUT cuts and
``ODIN3_PROV_UID=1`` (the record IDs are the hint keys); VPR runs for seed 1, or for seeds 1..N with
``--hint-seeds N``, and writes its routed net timing. ``timing_hints`` turns that into
``hints.tsv``; pass 2 is the same flow again with ``read_hints`` before the abc pass. The result
is ``two-pass.json`` (pass 1 and pass 2 side by side). Standard library only.
"""

from __future__ import annotations

import dataclasses
import json
import os
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

import timing_hints

PROV_DEPTH_FOR_HINTS = "3"  # the lowest provenance depth with an exact cut for each LUT
NET_TIMING = "report_net_timing.csv"
KEYS = ("crit_path_ns", "luts", "wirelength", "min_chan_width", "clbs")


def pick(summary: dict[str, Any]) -> dict[str, Any]:
    return {key: summary.get(key) for key in KEYS}


def pass_one_reports(args: Any, top: str, result: Any, seed_run: Callable[..., Any]
                     ) -> tuple[list[Path], list[float]]:
    """The net timing reports and routed CPDs of pass 1: seed 1 from the flow's own VPR run, the
    others from extra VPR runs of the same BLIF."""
    out: Path = args.out
    run_dir = result.vpr_run_dir
    cpd = result.summary.get("crit_path_ns")
    if run_dir is None or cpd is None or not (run_dir / NET_TIMING).is_file():
        raise timing_hints.HintError(f"pass 1 left no {NET_TIMING} (VPR did not route, or "
                                     f"--generate_net_timing_report was not applied)")
    csvs, cpds = [run_dir / NET_TIMING], [float(cpd)]
    blif = out / "odin3" / f"{top}.blif"
    for seed in range(2, args.hint_seeds + 1):
        extra = dataclasses.replace(args, vpr_seed=seed)
        seed_dir, seed_cpd = seed_run(extra, blif, out / f"vpr-seed{seed}")
        if seed_dir is None or seed_cpd is None or not (seed_dir / NET_TIMING).is_file():
            raise timing_hints.HintError(f"VPR seed {seed} of pass 1 did not give a net timing "
                                         f"report")
        csvs.append(seed_dir / NET_TIMING)
        cpds.append(seed_cpd)
    return csvs, cpds


def run(args: Any, top: str, run_flow: Callable[[Any], Any], seed_run: Callable[..., Any]) -> Any:
    """Pass 1, the hint file, pass 2. Returns pass 2's FlowResult with the comparison in its
    summary; ``ok`` only when both passes and the hint file succeeded."""
    out: Path = args.out
    level = args.prov_level or PROV_DEPTH_FOR_HINTS
    base = dataclasses.replace(args, two_pass=False, prov_level=level)
    old = os.environ.get("ODIN3_PROV_UID")
    os.environ["ODIN3_PROV_UID"] = "1"
    try:
        one = run_flow(dataclasses.replace(base, out=out / "pass1", net_timing=True))
        if not one.ok:
            one.summary["two_pass"] = "pass 1 failed"
            return one
        blif = out / "pass1" / "odin3" / f"{top}.blif"
        seeds_args = dataclasses.replace(base, out=out / "pass1")
        try:
            csvs, cpds = pass_one_reports(seeds_args, top, one, seed_run)
            meta = {"design": top, "vpr_seeds": ",".join(str(i + 1) for i in range(len(csvs)))}
            text, stats, _ = timing_hints.build(blif, blif.with_name(blif.name + ".names"), csvs,
                                                cpds, args.hint_cut, args.hint_max_cells,
                                                timing_hints.DEFAULT_TIE_SEED, meta)
        except (timing_hints.HintError, OSError, ValueError) as exc:
            print(f"odin3_flow: --two-pass: {exc}", file=sys.stderr)
            one.ok = False
            one.summary["two_pass"] = f"hint file failed: {exc}"
            return one
        hints = out / "hints.tsv"
        hints.write_text(text)
        two = run_flow(dataclasses.replace(base, out=out / "pass2", hints=hints))
    finally:
        if old is None:
            os.environ.pop("ODIN3_PROV_UID", None)
        else:
            os.environ["ODIN3_PROV_UID"] = old
    summary = {"pass1": pick(one.summary), "pass2": pick(two.summary), "hints": "hints.tsv",
               "hint_cells": stats.cells, "hint_seeds": len(cpds), "hint_cut": args.hint_cut}
    c1, c2 = summary["pass1"]["crit_path_ns"], summary["pass2"]["crit_path_ns"]
    if c1 and c2:
        summary["cpd_change_percent"] = round((c2 - c1) / c1 * 100.0, 3)
    (out / "two-pass.json").write_text(json.dumps(summary, indent=2) + "\n")
    two.summary["two_pass"] = summary
    return two
