"""The activity pass (spec docs/specs/2026-11-08-activity-design.md): hand-derived circuits, the
independent reference on random covers, sequential micros with known answers, the primary input
sources and the .act format. Run through CTest `activity_unit` (ODIN3_BIN set); skipped without it.
"""

from __future__ import annotations

import importlib.util
import json
import os
import random
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import ModuleType

REPO_ROOT = Path(__file__).resolve().parents[2]
ODIN3 = os.environ.get("ODIN3_BIN")
LIB = REPO_ROOT / "lib" / "vtr.o3lib"


def load(name: str) -> ModuleType:
    path = REPO_ROOT / "tools" / "activity" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def blif(body: str, inputs: str, outputs: str) -> str:
    return f".model t\n.inputs {inputs}\n.outputs {outputs}\n{body}.end\n"


COUNTER = """.latch d0 q0 re clk 0
.latch d1 q1 re clk 0
.latch d2 q2 re clk 0
.latch d3 q3 re clk 0
.names q0 d0
0 1
.names q0 q1 d1
10 1
01 1
.names q0 q1 c2
11 1
.names c2 q2 d2
10 1
01 1
.names q0 q1 q2 c3
111 1
.names c3 q3 d3
10 1
01 1
"""


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest activity_unit)")
class ActivityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def run_pass(self, text: str, flags: str = "") -> dict[str, tuple[float, float]]:
        """Per net (P1, density) from the .act file; JSON in self.raw, log in self.log."""
        src = self.dir / "t.blif"
        src.write_text(text)
        act = self.dir / "t.act"
        js = self.dir / "t.json"
        script = f"read_techlib {LIB}; read_blif {src}; activity {flags} --out {act} --json {js}"
        done = subprocess.run([str(ODIN3), "-p", script], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.log = done.stderr + done.stdout
        self.raw = json.loads(js.read_text())
        self.act_lines = act.read_text().splitlines()
        out = {}
        for line in self.act_lines:
            name, p1, dens = line.split()
            out[name] = (float(p1), float(dens))
        return out

    def check(
        self, got: dict[str, tuple[float, float]], want: dict[str, tuple[float, float]]
    ) -> None:
        for name, (p1, dens) in want.items():
            self.assertAlmostEqual(got[name][0], p1, places=7, msg=f"P1 of {name}")
            self.assertAlmostEqual(got[name][1], dens, places=7, msg=f"density of {name}")

    def test_and_xor_or_not(self) -> None:
        text = blif(
            ".names a b y\n11 1\n.names a b x\n10 1\n01 1\n"
            ".names a b o\n1- 1\n-1 1\n.names a n\n0 1\n",
            "a b",
            "y x o n",
        )
        got = self.run_pass(text)
        self.check(got, {"y": (0.25, 0.18), "x": (0.5, 0.32), "o": (0.75, 0.18), "n": (0.5, 0.2)})

    def test_options_and_ref(self) -> None:
        ref = load("act_ref")
        text = blif(".names a b c m\n0-1 1\n11- 1\n.names m c z\n10 1\n01 1\n", "a b c", "z")
        got = self.run_pass(text, "--pi-prob 0.3 --pi-density 0.4")
        self.check(got, ref.reference(text, 0.3, 0.4))

    def test_random_covers_equal_reference(self) -> None:
        ref = load("act_ref")
        rng = random.Random(7)
        for _ in range(12):
            width = rng.randint(2, 6)
            ins = [f"i{k}" for k in range(width)]
            rows = []
            for _ in range(rng.randint(1, 6)):
                rows.append("".join(rng.choice("01-") for _ in range(width)) + " 1")
            body = f".names {' '.join(ins)} y\n" + "\n".join(rows) + "\n"
            text = blif(body, " ".join(ins), "y")
            self.check(self.run_pass(text), ref.reference(text))

    def test_duplicate_pin_and_constant(self) -> None:
        text = blif(".names a a x\n10 1\n01 1\n.names one\n1\n.names a one y\n11 1\n", "a", "x y")
        got = self.run_pass(text)
        self.assertEqual(got["x"], (0.0, 0.0))
        self.assertAlmostEqual(got["y"][0], 0.5)
        self.assertAlmostEqual(got["y"][1], 0.2)

    def test_shift_register(self) -> None:
        text = blif(
            ".latch din q1 re clk 0\n.latch q1 q2 re clk 0\n.latch q2 q3 re clk 0\n",
            "clk din",
            "q3",
        )
        got = self.run_pass(text)
        for name in ("q1", "q2", "q3"):
            self.check(got, {name: (0.5, 0.2)})
        self.check(got, {"clk": (0.5, 2.0)})

    def test_counter(self) -> None:
        got = self.run_pass(blif(COUNTER, "clk", "q3"))
        self.check(got, {"q0": (0.5, 1.0), "q1": (0.5, 0.5), "q2": (0.5, 0.25), "q3": (0.5, 0.125)})
        self.assertTrue(self.raw["converged"])

    def test_toggle_flop(self) -> None:
        text = blif(".latch d q re clk 0\n.names q d\n0 1\n", "clk", "q")
        self.check(self.run_pass(text), {"q": (0.5, 1.0)})

    def test_enable_register(self) -> None:
        # q <= e ? d : q : P(next != q) = P(e) * P(d != q) = 0.25 for inputs (0.5, 0.2)
        text = blif(".latch nq q re clk 0\n.names e d q nq\n11- 1\n0-1 1\n", "clk e d", "q")
        got = self.run_pass(text)
        self.assertAlmostEqual(got["q"][0], 0.5, places=6)
        self.assertAlmostEqual(got["q"][1], 0.25, places=6)

    def test_absorbing_state_and_ff_start(self) -> None:
        text = blif(".latch nq q re clk 0\n.names q x nq\n11 1\n", "clk x", "q")
        got = self.run_pass(text)
        self.assertLess(got["q"][0], 1e-5)
        got = self.run_pass(text, "--ff-start 0")
        self.assertEqual(got["q"][0], 0.0)

    def test_pi_file(self) -> None:
        pif = self.dir / "pi.txt"
        pif.write_text("a 0.8 0.1\nb 0.2 0.2\n")
        text = blif(".names a b y\n11 1\n", "a b", "y")
        got = self.run_pass(text, f"--pi-file {pif}")
        self.check(got, {"a": (0.8, 0.1), "b": (0.2, 0.2)})
        self.assertEqual(self.raw["inputs"]["mode"], "file")

    def test_pi_file_invalid_density(self) -> None:
        pif = self.dir / "pi.txt"
        pif.write_text("a 0.1 0.9\n")
        src = self.dir / "t.blif"
        src.write_text(blif(".names a y\n1 1\n", "a", "y"))
        done = subprocess.run(
            [str(ODIN3), "-p", f"read_techlib {LIB}; read_blif {src}; activity --pi-file {pif}"],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(done.returncode, 0)

    def test_vcd_inputs(self) -> None:
        # clk rises at t=10,20,30,40,50; a is sampled before each rising edge: 0,1,1,0,0
        vcd = self.dir / "w.vcd"
        vcd.write_text(
            "$timescale 1ns $end\n$scope module tb $end\n$var wire 1 ! clk $end\n"
            '$var wire 1 " a $end\n'
            '$upscope $end\n$enddefinitions $end\n#0\n0!\n0"\n#5\n1"\n#10\n1!\n#15\n0!\n#20\n1!\n'
            '#25\n0!\n#30\n1!\n#32\n0"\n#35\n0!\n#40\n1!\n#45\n0!\n#50\n1!\n'
        )
        text = blif(".latch a q re clk 0\n", "clk a", "q")
        got = self.run_pass(text, f"--vcd {vcd} --vcd-clock clk")
        self.assertEqual(self.raw["inputs"]["mode"], "vcd")
        # samples before edges: a = 1, 1, 1, 0, 0 -> ones 3 of 5, 1 change of 4
        self.check(got, {"a": (0.6, 0.25)})

    def test_act_format_one_line_per_net(self) -> None:
        got = self.run_pass(blif(COUNTER, "clk", "q3"))
        names = [line.split()[0] for line in self.act_lines]
        self.assertEqual(len(names), len(set(names)))
        self.assertIn("clk", names)
        self.assertEqual(len(got), len(names))

    def test_matches_markov_simulation(self) -> None:
        """No reconvergence: the pass equals a long simulation of independent two-state chains."""
        text = blif(
            ".names a b t\n11 1\n.names t c u\n10 1\n01 1\n.names u d y\n1- 1\n-1 1\n",
            "a b c d",
            "y",
        )
        got = self.run_pass(text, "--pi-prob 0.4 --pi-density 0.3")
        rng = random.Random(11)
        prob, dens = 0.4, 0.3
        rise, fall = dens / 2 / (1 - prob), dens / 2 / prob  # P(0->1), P(1->0) per cycle
        state = [rng.random() < prob for _ in range(4)]
        cycles = 200000
        ones = toggles = 0
        last = None
        for _ in range(cycles):
            state = [(rng.random() >= fall) if bit else (rng.random() < rise) for bit in state]
            a, b, c, d = state
            out = ((a and b) != c) or d
            ones += out
            toggles += last is not None and out != last
            last = out
        self.assertAlmostEqual(got["y"][0], ones / cycles, delta=0.01)
        self.assertAlmostEqual(got["y"][1], toggles / cycles, delta=0.01)

    def test_golden_micros(self) -> None:
        """tests/activity/*.blif against the committed .act files (values checked by hand above)."""
        for src in sorted((REPO_ROOT / "tests" / "activity").glob("*.blif")):
            want = {}
            for line in src.with_suffix(".act.golden").read_text().splitlines():
                name, p1, dens = line.split()
                want[name] = (float(p1), float(dens))
            got = self.run_pass(src.read_text())
            self.assertEqual(sorted(got), sorted(want), src.name)
            self.check(got, want)

    def test_summary_attributes_and_hash_names(self) -> None:
        src = self.dir / "t.blif"
        src.write_text(blif(".names a b y\n11 1\n", "a b", "y"))
        done = subprocess.run(
            [
                str(ODIN3),
                "-p",
                f"read_techlib {LIB}; read_blif {src}; "
                f"activity --name-style hash --out {self.dir / 'h.act'}",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertGreaterEqual(len((self.dir / "h.act").read_text().splitlines()), 3)


if __name__ == "__main__":
    unittest.main()
