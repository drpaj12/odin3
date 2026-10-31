#!/usr/bin/env python3
"""Gate: every net of a combinational netlist, simulated by the Odin III Simulation API, equals an
independent evaluator on random vectors (spec docs/specs/2026-10-31-netlist-edit-sim-design.md 5).

The independent evaluator is `lemanruss.htl.blif_sim.evaluate_cycle` (Leman Russ III, pure Python,
`.names` only). It is an external oracle named by --oracle (the Leman Russ repo root) and is never
vendored into the core. Inputs are BLIF files, or Verilog files that are first lowered with
`read_verilog; proc; opt; lower; write_blif --gates-as-names`. Designs with .latch or .subckt are
skipped (the oracle is combinational) and reported as skipped.

Usage: check_vs_oracle.py [--oracle PATH | $ODIN3_LEMANRUSS] [--vectors N] [--seed S]
       [--odin3 BIN] file...
Needs ODIN3_LIB (libodin3.so) in the environment, as the Python binding does.
Exit 0 when every simulated net equals the oracle, 1 on a mismatch, 2 on a usage error, 77 when
nothing could be compared or the oracle is missing (CTest skip).
"""

from __future__ import annotations

import argparse
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SKIP = 77


def lower_verilog(odin3: str, source: Path, out: Path) -> bool:
    script = f"read_verilog {source}; proc; opt; lower; write_blif --gates-as-names {out}"
    done = subprocess.run([odin3, "-p", script], capture_output=True, text=True, check=False)
    return done.returncode == 0 and out.exists()


def check_one(blif: Path, vectors: int, seed: int) -> tuple[str, int]:
    """Returns (status, nets compared): status is ok, skipped:<why> or mismatch:<detail>."""
    from lemanruss.htl.blif_sim import evaluate_cycle  # type: ignore[import-not-found]
    from lemanruss.htl.blif_text import parse_single_model  # type: ignore[import-not-found]

    import odin3

    text = blif.read_text()
    lines = text.splitlines()
    if any(line.startswith((".latch", ".subckt", ".gate")) for line in lines):
        return "skipped:sequential or hierarchical", 0
    if sum(line.startswith(".model") for line in lines) > 1:
        return "skipped:several models (the oracle reads one)", 0
    try:
        model = parse_single_model(text)
        evaluate_cycle(model, dict.fromkeys(model.inputs, 0))
    except ValueError as err:
        return f"skipped:oracle cannot read it ({err})", 0
    rng = random.Random(seed)
    design = odin3.Design()
    try:
        design.run_pass("read_blif", str(blif))
        sim = design.simulate()
        compared: set[str] = set()
        for _ in range(vectors):
            inputs = {name: rng.getrandbits(1) for name in model.inputs}
            want = evaluate_cycle(model, inputs)
            for name, value in inputs.items():
                if sim.signal(name) is not None:  # an unused input may have no net
                    sim.set(name, value)
            sim.settle()
            for name, value in want.items():
                if sim.signal(name) is None:
                    return f"mismatch:{blif.name}: net {name} is not visible", len(compared)
                got = sim.get(name)
                if got != value:
                    return f"mismatch:{blif.name}: net {name} sim {got} oracle {value}", len(
                        compared
                    )
                compared.add(name)
        sim.close()
        return "ok", len(compared)
    finally:
        design.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument(
        "--oracle",
        default=os.environ.get("ODIN3_LEMANRUSS", ""),
        help="Leman Russ repo root (has lemanruss/htl); default $ODIN3_LEMANRUSS",
    )
    ap.add_argument("--vectors", type=int, default=64)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--odin3", default=str(REPO / "build" / "debug" / "odin3"))
    ap.add_argument("files", nargs="+", type=Path)
    args = ap.parse_args()
    oracle = Path(args.oracle)
    if not (oracle / "lemanruss" / "htl" / "blif_sim.py").exists():
        print(f"skipped: no oracle at {oracle}")
        return SKIP
    sys.path.insert(0, str(oracle))
    sys.path.insert(0, str(REPO / "plugins" / "python"))
    failed = 0
    compared_total = 0
    with tempfile.TemporaryDirectory() as tmp:
        for path in args.files:
            blif = path
            if path.suffix == ".v":
                blif = Path(tmp) / (path.stem + ".blif")
                if not lower_verilog(args.odin3, path, blif):
                    print(f"{path.name}: skipped:lowering failed")
                    continue
            status, count = check_one(blif, args.vectors, args.seed)
            print(f"{path.name}: {status} ({count} nets)")
            compared_total += count
            failed += status.startswith("mismatch")
    if failed:
        return 1
    return 0 if compared_total else SKIP


if __name__ == "__main__":
    sys.exit(main())
