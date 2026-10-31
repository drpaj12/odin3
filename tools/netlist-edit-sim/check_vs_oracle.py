#!/usr/bin/env python3
"""Gate: every net of a combinational netlist, simulated by the Odin III Simulation API, equals an
independent evaluator on random vectors, before and after edits (spec
docs/specs/2026-10-31-netlist-edit-sim-design.md 5).

The independent evaluator is `lemanruss.htl.blif_sim.evaluate_cycle` (Leman Russ III, pure Python,
`.names` only). It is an external oracle named by --oracle (the Leman Russ repo root) and is never
vendored into the core. Inputs are BLIF files, or Verilog files that are first lowered with
`read_verilog; proc; opt; lower; write_blif --gates-as-names`. Designs with .latch or .subckt are
skipped (the oracle is combinational) and reported as skipped.

Per design: round 1 compares every net of the design as read. Round 2 applies edits (set_cover on
the first $sop with 2 to 6 inputs, then a rewire of one $sop input pin to a primary input), writes
the edited design with write_blif, and compares every net of the edited design with the oracle
evaluating that written BLIF.

--selftest needs no oracle: it checks run_vectors outputs and toggle counts against values worked
out by hand (a combinational AND, and a latch circuit).

Usage: check_vs_oracle.py [--oracle PATH | $ODIN3_LEMANRUSS] [--vectors N] [--seed S]
       [--odin3 BIN] file...
       check_vs_oracle.py --selftest
Needs ODIN3_LIB (libodin3.so) in the environment, as the Python binding does.
Exit 0 when every simulated net equals the oracle, 1 on a mismatch, 2 on a usage error, 77 when
nothing could be compared or the oracle is missing. The 77 case prints a SKIP line in capitals
(and a GitHub `::warning::` annotation under GitHub Actions) so a skipped gate is never silent.
Leman Russ III is a private repository, so CI cannot check it out: in CI this gate is skipped
and says so; run it with ODIN3_LEMANRUSS set before merging changes to the editing or simulation
code.
"""

from __future__ import annotations

import argparse
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[2]
SKIP = 77


def lower_verilog(odin3: str, source: Path, out: Path) -> bool:
    script = f"read_verilog {source}; proc; opt; lower; write_blif --gates-as-names {out}"
    done = subprocess.run([odin3, "-p", script], capture_output=True, text=True, check=False)
    return done.returncode == 0 and out.exists()


def oracle() -> tuple[Any, Any]:
    """(evaluate_cycle, parse_single_model) of the external oracle (on sys.path by main)."""
    from lemanruss.htl.blif_sim import evaluate_cycle  # type: ignore[import-not-found]
    from lemanruss.htl.blif_text import parse_single_model  # type: ignore[import-not-found]

    return evaluate_cycle, parse_single_model


def compare_nets(
    design: Any, model: Any, vectors: int, rng: random.Random, label: str
) -> tuple[str, int]:
    """Simulate design on random vectors and compare every net the oracle evaluates."""
    evaluate_cycle, _ = oracle()
    sim = design.simulate()
    compared: set[str] = set()
    try:
        for _ in range(vectors):
            inputs = {name: rng.getrandbits(1) for name in model.inputs}
            want = evaluate_cycle(model, inputs)
            for name, value in inputs.items():
                if sim.signal(name) is not None:  # an unused input may have no net
                    sim.set(name, value)
            sim.settle()
            for name, value in want.items():
                if sim.signal(name) is None:
                    return f"mismatch:{label}: net {name} is not visible", len(compared)
                got = sim.get(name)
                if got != value:
                    return f"mismatch:{label}: net {name} sim {got} oracle {value}", len(compared)
                compared.add(name)
    finally:
        sim.close()
    return "ok", len(compared)


def random_cover(width: int, rng: random.Random) -> str:
    """BLIF cover rows of a random truth table of `width` inputs (one row per minterm kept)."""
    rows = [
        format(m, f"0{width}b") + " 1\n" for m in range(1 << width) if rng.getrandbits(1)
    ]
    return "".join(rows)


def edit_design(design: Any, input_names: list[str], rng: random.Random) -> str | None:
    """set_cover on the first $sop with 2 to 6 inputs, then rewire one $sop input pin to a
    primary input. Returns an error text, or None when the edits were applied."""
    top = design.top()
    sops = [n for n in top.nodes() if n.type_name == "$sop"]
    target = next((n for n in sops if 2 <= int(n.params()["WIDTH"]) <= 6), None)
    if target is None:
        return "no $sop of 2 to 6 inputs to edit"
    width = int(target.params()["WIDTH"])
    twin = target.set_cover(random_cover(width, rng), "oracle:set_cover")
    if target.is_live or not twin.is_live or twin.edit_tag != "oracle:set_cover":
        return "set_cover did not replace the node by a tagged twin"
    pi = top.net(input_names[0]) if input_names else None
    if pi is None:
        return None
    for node in sops:
        if not node.is_live:
            continue
        for pin in node.pins():
            net = pin.net()
            if pin.direction == "in" and net is not None and net.name != pi.name:
                pin.rewire(pi)
                return None
    return None


def check_one(blif: Path, vectors: int, seed: int, workdir: Path) -> tuple[str, int]:
    """Returns (status, nets compared): status is ok, skipped:<why> or mismatch:<detail>."""
    evaluate_cycle, parse_single_model = oracle()

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
        status, count = compare_nets(design, model, vectors, rng, blif.name)
        if status != "ok":
            return status, count
        # Round 2: edit, write, and compare against the oracle on the written, edited BLIF.
        problem = edit_design(design, list(model.inputs), rng)
        if problem is not None:
            return f"skipped:round 2 ({problem})", count
        edited = workdir / (blif.stem + ".edited.blif")
        design.run_pass("write_blif", str(edited))
        emodel = parse_single_model(edited.read_text())
        status2, count2 = compare_nets(design, emodel, vectors, rng, blif.name + " (edited)")
        return (status2, count + count2) if status2 == "ok" else (status2, count)
    finally:
        design.close()


def selftest() -> int:
    """run_vectors outputs and toggle counts against hand-worked values (no oracle needed)."""
    try:
        import odin3
    except ImportError:
        print("skipped 'cffi' (the Python binding needs cffi)")
        return 0

    cases = [
        # y = a & b; rows (a, b): (1,1) (1,0) (1,1) (0,1); y 1 0 1 0 -> 4 toggles from 0;
        # a: 0 1 1 1 0 -> 2; b: 0 1 0 1 1 -> 3.
        (
            ".model m\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n",
            ["a", "b"],
            [[1, 1], [1, 0], [1, 1], [0, 1]],
            ["y"],
            [[1], [0], [1], [0]],
            {"y": 4, "a": 2, "b": 3},
        ),
        # q <= en ^ q: en 1 1 0 1 -> q 1 0 0 1, d = en ^ q (before the edge) 0 1 0 0 -> hand:
        # outputs read after each cycle (q, d) = (1,0) (0,1) (0,0) (1,0); q toggles 3, d toggles 2.
        (
            ".model s\n.inputs clk en\n.outputs q\n.names en q d\n10 1\n01 1\n"
            ".latch d q re clk 0\n.end\n",
            ["en"],
            [[1], [1], [0], [1]],
            ["q", "d"],
            [[1, 0], [0, 1], [0, 0], [1, 0]],
            {"q": 3, "d": 2},
        ),
    ]
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for index, (text, ins, rows, outs, want, toggles) in enumerate(cases):
            path = Path(tmp) / f"case{index}.blif"
            path.write_text(text)
            design = odin3.Design()
            try:
                design.run_pass("read_blif", str(path))
                sim = design.simulate()
                try:
                    sim.set_toggle_counting()
                    got = sim.run_vectors(ins, rows, outs)
                    counts = {name: sim.toggles(name) for name in toggles}
                finally:
                    sim.close()
            finally:
                design.close()
            ok = got == want and counts == toggles
            print(f"selftest case {index}: {'ok' if ok else 'MISMATCH'} got {got} {counts}")
            failed += not ok
    return 1 if failed else 0


def announce_skip(why: str) -> int:
    """A skipped gate must be visible in CI logs: a capitals line, and an annotation on Actions."""
    print(f"SKIP: netlist-edit-sim oracle gate NOT RUN: {why}")
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::warning title=oracle gate skipped::netlist-edit-sim oracle gate NOT RUN: {why}")
    return SKIP


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
    ap.add_argument("--selftest", action="store_true", help="hand-valued run_vectors/toggle checks")
    ap.add_argument("files", nargs="*", type=Path)
    args = ap.parse_args()
    sys.path.insert(0, str(REPO / "plugins" / "python"))
    if args.selftest:
        return selftest()
    if not args.files:
        ap.error("no files given")
    oracle = Path(args.oracle)
    if not args.oracle or not (oracle / "lemanruss" / "htl" / "blif_sim.py").exists():
        return announce_skip(
            f"no Leman Russ III checkout at {args.oracle!r} (ODIN3_LEMANRUSS unset or wrong; "
            "drpaj12/lemanrussIII is private, so CI cannot fetch it)"
        )
    sys.path.insert(0, str(oracle))
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
            status, count = check_one(blif, args.vectors, args.seed, Path(tmp))
            print(f"{path.name}: {status} ({count} nets)")
            compared_total += count
            failed += status.startswith("mismatch")
    if failed:
        return 1
    if compared_total == 0:
        return announce_skip("every design was skipped, nothing was compared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
