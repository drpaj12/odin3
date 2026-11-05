"""gate_sim: simulate the flow's ``final.v`` with the sky130 models and a testbench, reset-aware.

The silicon flow netlists many registers that the RTL never resets (they are loaded before
use). A gate netlist can then carry an X that the RTL simulation never shows (X pessimism: the
logic is equal for 0 and for 1 on the unknown, but a gate-level simulator cannot see that).
This tool runs the testbench three times, with every flip-flop and latch of the sky130 UDP models
starting at X (as the models ship), at 0 and at 1. Verdict:

A UDP shows its initial state on its output at its first input event, so a flip-flop whose
input never changes before the first clock edge starts at X in all three runs.

* ``pass``: the plain run (X start) passes.
* ``x-pessimism``: the X run fails, and the 0 run and the 1 run both pass: the difference is the
  unknown start state, not the function.
* ``fail``: the 0 run or the 1 run fails: a functional difference (or a reset bug).

Usage: gate_sim.py FINAL.V --pdk-verilog DIR --tb-top TOP --tb FILE... [--pass TEXT]
[-I DIR] [--param NAME=VALUE] [--out DIR]. Exit 0 for pass and x-pessimism, 1 for fail, 2 usage.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

BAD_LINE = re.compile(r"^FAIL|ERROR:|Data compare error", re.M)
SEQ_UDP = re.compile(r"primitive\s+sky130_fd_sc_hd__udp_d(?:ff|latch)\S*\s*\(.*?\breg\s+Q\s*;",
                     re.S)


def patch_primitives(text: str, init: str) -> str:
    """The sky130 primitives with every flip-flop and latch UDP starting at ``init`` (0 or 1)."""
    if init not in ("0", "1"):
        raise ValueError(f"init must be 0 or 1, not {init!r}")
    return SEQ_UDP.sub(lambda m: f"{m.group(0)}\n    initial Q = 1'b{init};", text)


def strip_param(tb: str, name: str) -> str:
    """The testbench text without ``#(.NAME(value))`` on module instances.

    A flow netlist has no parameters; the value the testbench passes was built into it
    (silicon_flow ``--param``), so the override is dropped here."""
    return re.sub(rf"#\s*\(\s*\.{re.escape(name)}\s*\([^()]*\)\s*\)", "", tb)


def verdict(results: dict[str, bool]) -> str:
    """``pass``, ``x-pessimism`` or ``fail`` from the pass flag of the runs x, 0 and 1."""
    if results["x"]:
        return "pass"
    return "x-pessimism" if results["0"] and results["1"] else "fail"


def run_one(work: Path, final: Path, models: Sequence[Path], tb: Sequence[Path], top: str,
            pass_text: str, incs: Sequence[Path], timeout: int) -> dict[str, object]:
    """Compile and run one simulation in ``work``; the dict has ``ok``, ``checks`` and the log."""
    work.mkdir(parents=True, exist_ok=True)
    vvp = work / "sim.vvp"
    cmd = ["iverilog", "-g2005", "-DFUNCTIONAL", "-DUNIT_DELAY=#0", "-s", top, "-o", str(vvp)]
    for inc in incs:
        cmd += ["-I", str(inc)]
    cmd += [str(p) for p in tb] + [str(final)] + [str(m) for m in models]
    comp = subprocess.run(cmd, capture_output=True, text=True, check=False)
    (work / "iv.log").write_text(comp.stdout + comp.stderr)
    if comp.returncode != 0:
        return {"ok": False, "reason": "iverilog failed; see iv.log"}
    try:
        sim = subprocess.run(["vvp", "-n", str(vvp)], capture_output=True, text=True,
                             stdin=subprocess.DEVNULL, timeout=timeout, check=False)
    except subprocess.TimeoutExpired:
        return {"ok": False, "reason": f"timeout after {timeout} s"}
    text = sim.stdout + sim.stderr
    (work / "sim.log").write_text(text)
    bad = len(BAD_LINE.findall(text))
    ok = sim.returncode == 0 and pass_text in text and bad == 0
    return {"ok": ok, "bad_lines": bad, "reason": "" if ok else "pass text missing or FAIL lines"}


def simulate(final: Path, pdk_verilog: Path, tb: Sequence[Path], top: str, out: Path,
             pass_text: str = "PASS", incs: Sequence[Path] = (),
             params: Sequence[str] = (), timeout: int = 1800) -> dict[str, object]:
    """The three runs and the verdict; written to ``out/gate_sim.json``."""
    out.mkdir(parents=True, exist_ok=True)
    prim = (pdk_verilog / "primitives.v").read_text()
    cells = pdk_verilog / "sky130_fd_sc_hd.v"
    tbs: list[Path] = []
    for src in tb:
        text = src.read_text()
        for p in params:
            text = strip_param(text, p.partition("=")[0])
        dst = out / "tb" / src.name
        dst.parent.mkdir(exist_ok=True)
        dst.write_text(text)
        tbs.append(dst)
    runs: dict[str, dict[str, object]] = {}
    for mode in ("x", "0", "1"):
        models = [pdk_verilog / "primitives.v", cells]
        if mode != "x":
            patched = out / f"primitives_init{mode}.v"
            patched.write_text(patch_primitives(prim, mode))
            models = [patched, cells]
        runs[mode] = run_one(out / f"run_{mode}", final, models, tbs, top, pass_text,
                             [*incs, out / "tb"], timeout)
    flags = {k: bool(v["ok"]) for k, v in runs.items()}
    result: dict[str, object] = {"verdict": verdict(flags), "runs": runs}
    (out / "gate_sim.json").write_text(json.dumps(result, indent=1, sort_keys=True) + "\n")
    return result


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="gate_sim", description=(__doc__ or "").split("\n")[0])
    ap.add_argument("final", type=Path)
    ap.add_argument("--pdk-verilog", type=Path, required=True,
                    help="libs.ref/sky130_fd_sc_hd/verilog of the PDK")
    ap.add_argument("--tb-top", required=True)
    ap.add_argument("--tb", type=Path, nargs="+", required=True)
    ap.add_argument("--pass", dest="pass_text", default="PASS")
    ap.add_argument("-I", dest="incs", type=Path, action="append", default=[])
    ap.add_argument("--param", action="append", default=[], metavar="NAME=VALUE",
                    help="drop the testbench's #(.NAME(..)) override (built in by the flow)")
    ap.add_argument("--out", type=Path, default=Path("gate_sim_out"))
    ap.add_argument("--timeout", type=int, default=1800)
    ns = ap.parse_args(list(sys.argv[1:] if argv is None else argv))
    res = simulate(ns.final.resolve(), ns.pdk_verilog, [p.resolve() for p in ns.tb], ns.tb_top,
                   ns.out.resolve(), ns.pass_text, [p.resolve() for p in ns.incs], ns.param,
                   ns.timeout)
    print(f"gate_sim: {res['verdict']}")
    if res["verdict"] == "x-pessimism":
        print("WARNING gate_sim: the run from an X start state fails; starting every flip-flop at "
              "0 and at 1 both pass: X pessimism of the gate simulation, not a functional "
              "difference", file=sys.stderr)
    return 1 if res["verdict"] == "fail" else 0


if __name__ == "__main__":
    sys.exit(main())
