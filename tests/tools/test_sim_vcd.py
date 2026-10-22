"""S1 plan Task 9 (spec SW-3, SW-4): the VCD of ``sim --vcd`` against the vector file it is
written with, and against Icarus Verilog driven with the same inputs at the SW-3 times. Needs
``ODIN3_CLI`` (the Icarus test also ``iverilog`` and ``vvp``). Work directories are removed when
each test ends."""

from __future__ import annotations

import os
import random
import shutil
import subprocess
import unittest
from pathlib import Path

from . import helpers

ODIN3 = os.environ.get("ODIN3_CLI", "")
ROOT = Path(__file__).resolve().parents[2]
TRAFFIC = ROOT / "tests" / "sim" / "traffic.v"
PERIOD = 10
CYCLES = 24
CHANGES = dict[int, dict[str, str]]


def read_vcd(path: Path) -> tuple[dict[str, str], CHANGES]:
    """The VCD's first variable of each name -> its code, and {time: {code: bits}} per ``#t``."""
    codes: dict[str, str] = {}
    changes: CHANGES = {}
    now = -1
    for line in path.read_text().splitlines():
        word = line.split()
        if not word:
            continue
        if word[0] == "$var":
            codes.setdefault(word[4], word[3])
        elif word[0].startswith("#"):
            now = int(word[0][1:])
            changes.setdefault(now, {})
        elif now >= 0 and word[0][0] in "01xz" and len(word) == 1:
            changes[now][word[0][1:]] = word[0][0]
        elif now >= 0 and word[0][0] == "b" and len(word) == 2:
            changes[now][word[1]] = word[0][1:]
    return codes, changes


def value_at(changes: CHANGES, code: str, time: int) -> str:
    """The value of ``code`` at ``time`` (the last change at or before it)."""
    value = ""
    for stamp in sorted(changes):
        if stamp > time:
            break
        value = changes[stamp].get(code, value)
    return value


def run_odin3(script: str, cwd: Path) -> None:
    done = subprocess.run([ODIN3, "-p", script], capture_output=True, text=True, cwd=cwd,
                          check=False, timeout=300)
    if done.returncode != 0:
        raise AssertionError(f"odin3 exit {done.returncode}: {(done.stdout + done.stderr)[-400:]}")


@unittest.skipUnless(ODIN3 and Path(ODIN3).exists(), "odin3 (ODIN3_CLI) is needed")
class VcdTest(unittest.TestCase):
    pre = f"read_verilog {TRAFFIC}; proc; opt; lower"

    def test_vcd_matches_vectors(self) -> None:
        work = helpers.scratch(self, "test-sim-vcd-")
        run_odin3(f"{self.pre}; sim {CYCLES} 8 v.vec --vcd w.vcd", work)
        codes, changes = read_vcd(work / "w.vcd")
        lines = [ln.split() for ln in (work / "v.vec").read_text().splitlines()
                 if not ln.startswith("#")]
        self.assertEqual(len(lines), CYCLES)
        for cyc, (_, _, outputs) in enumerate(lines):
            self.assertEqual(value_at(changes, codes["light_ns"], PERIOD * cyc + PERIOD),
                             outputs[:2], f"light_ns, cycle {cyc}")
            self.assertEqual(value_at(changes, codes["light_ew"], PERIOD * cyc + PERIOD),
                             outputs[2:], f"light_ew, cycle {cyc}")
            for off, clock in ((2, "0"), (5, "1"), (PERIOD, "0")):
                self.assertEqual(value_at(changes, codes["clk"], PERIOD * cyc + off), clock,
                                 f"clk, cycle {cyc} +{off}")

    @unittest.skipUnless(shutil.which("iverilog") and shutil.which("vvp"),
                         "iverilog and vvp are needed")
    def test_vcd_matches_icarus(self) -> None:
        work = helpers.scratch(self, "test-sim-vcd-")
        rng = random.Random(8)
        inputs = ["100"] + [f"0{rng.randrange(2)}{rng.randrange(2)}" for _ in range(CYCLES - 1)]
        (work / "in.vec").write_text(
            "".join(f"{cyc} {bits}\n" for cyc, bits in enumerate(inputs)))
        run_odin3(f"{self.pre}; sim --vectors-in in.vec v.vec --vcd w.vcd", work)
        steps = "".join(
            f"        #2 {{rst, car_ns, car_ew}} = 3'b{bits};\n        #3 clk = 1;\n"
            "        #5 clk = 0;\n" for bits in inputs)
        (work / "tb.v").write_text(
            "`timescale 1ns/1ns\nmodule tb;\n    reg clk = 0, rst = 0, car_ns = 0, car_ew = 0;\n"
            "    wire [1:0] light_ns, light_ew;\n"
            "    traffic dut(.clk(clk), .rst(rst), .car_ns(car_ns), .car_ew(car_ew),\n"
            "                .light_ns(light_ns), .light_ew(light_ew));\n"
            "    initial begin\n        $dumpfile(\"ref.vcd\");\n        $dumpvars(0, dut);\n"
            f"{steps}        #1 $finish;\n    end\nendmodule\n")
        for cmd in (["iverilog", "-o", "sim.vvp", "tb.v", str(TRAFFIC)], ["vvp", "sim.vvp"]):
            done = subprocess.run(cmd, capture_output=True, text=True, cwd=work, check=False,
                                  timeout=120)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        ours, ours_changes = read_vcd(work / "w.vcd")
        ref, ref_changes = read_vcd(work / "ref.vcd")
        for cyc in range(CYCLES):
            when = PERIOD * cyc + PERIOD
            for name in ("light_ns", "light_ew"):
                # Icarus drops the leading zeros of a vector value; ours has all its digits.
                # An empty value (never dumped) is padded to nothing and fails the comparison.
                theirs = value_at(ref_changes, ref[name], when)
                theirs = theirs.rjust(2, "0") if theirs else theirs
                self.assertEqual(value_at(ours_changes, ours[name], when), theirs,
                                 f"{name}, cycle {cyc}")
            if "clk" in ref:
                for off in (2, 5, PERIOD):
                    self.assertEqual(value_at(ours_changes, ours["clk"], PERIOD * cyc + off),
                                     value_at(ref_changes, ref["clk"], PERIOD * cyc + off),
                                     f"clk, cycle {cyc} +{off}")


if __name__ == "__main__":
    unittest.main()
