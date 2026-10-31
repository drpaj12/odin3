"""The scoap pass (spec docs/specs/2026-10-31-scoap-design.md): hand-computed circuits, the
independent reference on small designs, the construction check and the signal probability against
exact enumeration. Run through CTest `scoap_unit` (ODIN3_BIN set); skipped without it.
"""

from __future__ import annotations

import importlib.util
import itertools
import json
import math
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import ModuleType

REPO_ROOT = Path(__file__).resolve().parents[2]
ODIN3 = os.environ.get("ODIN3_BIN")
LIB = REPO_ROOT / "lib" / "vtr.o3lib"
INF = 1_000_000_000


def load(name: str) -> ModuleType:
    path = REPO_ROOT / "tools" / "scoap" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def blif(body: str, inputs: str, outputs: str) -> str:
    return f".model t\n.inputs {inputs}\n.outputs {outputs}\n{body}.end\n"


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest scoap_unit)")
class ScoapTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def run_pass(self, text: str, flags: str = "") -> dict[str, dict[str, int]]:
        """The first module's nets by name, plus the raw JSON in self.raw."""
        src = self.dir / "t.blif"
        src.write_text(text)
        out = self.dir / "s.json"
        script = f"read_techlib {LIB}; read_blif {src}; scoap {flags} --out {out}"
        done = subprocess.run([str(ODIN3), "-p", script], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.raw = json.loads(out.read_text())
        self.log = done.stderr + done.stdout
        return {n["name"]: n for n in self.raw["modules"][0]["nets"]}

    def check(self, nets: dict[str, dict[str, int]], want: dict[str, dict[str, int]]) -> None:
        for name, vals in want.items():
            for key, val in vals.items():
                self.assertEqual(nets[name][key], val, f"{name}.{key}")

    def test_and_chain(self) -> None:
        text = blif(".names a b y\n11 1\n.names y c z\n11 1\n", "a b c", "z")
        got = self.run_pass(text)
        self.check(got, {
            "a": {"cc0": 1, "cc1": 1, "co": 4}, "c": {"co": 4},
            "y": {"cc0": 2, "cc1": 3, "co": 2}, "z": {"cc0": 2, "cc1": 5, "co": 0},
        })

    def test_or_not_xor(self) -> None:
        text = blif(
            ".names a b o\n1- 1\n-1 1\n.names a n\n0 1\n.names a b x\n10 1\n01 1\n",
            "a b", "o n x",
        )
        got = self.run_pass(text)
        self.check(got, {
            "o": {"cc0": 3, "cc1": 2}, "n": {"cc0": 2, "cc1": 2},
            "x": {"cc0": 3, "cc1": 3}, "a": {"co": 1},  # the NOT is cheapest
        })

    def test_observability_values(self) -> None:
        # co(a) through OR (b to 0: 1, +1) = 2; through XOR (min(cc0 b, cc1 b) = 1, +1) = 2
        got = self.run_pass(blif(".names a b o\n1- 1\n-1 1\n", "a b", "o"))
        self.check(got, {"a": {"co": 2}, "b": {"co": 2}})
        got = self.run_pass(blif(".names a b x\n10 1\n01 1\n", "a b", "x"))
        self.check(got, {"a": {"co": 2}, "b": {"co": 2}})

    def test_mux(self) -> None:
        # y = s ? b : a, primes of y=1: (s0,a1) (s1,b1) (a1,b1) -> cc1 = 1 + 2 = 3
        got = self.run_pass(blif(".names s a b y\n01- 1\n1-1 1\n", "s a b", "y"))
        self.check(got, {"y": {"cc0": 3, "cc1": 3}, "a": {"co": 3}, "s": {"co": 3}})

    def test_sop_off_set_cover(self) -> None:
        # OFF-set cover of a NAND: rows give 0 on a hit; y = NOT(a AND b): cc0 = 3, cc1 = 2
        got = self.run_pass(blif(".names a b y\n11 0\n", "a b", "y"))
        self.check(got, {"y": {"cc0": 3, "cc1": 2}})

    def test_constants_and_unreachable(self) -> None:
        text = blif(".names one\n1\n.names a one y\n11 1\n.names zero\n.names zero a q\n11 1\n",
                    "a", "y q")
        got = self.run_pass(text)
        self.check(got, {"one": {"cc0": INF, "cc1": 1}, "y": {"cc0": 2, "cc1": 3},
                         "q": {"cc1": INF}})
        self.assertGreaterEqual(self.log.count("unreachable"), 1)

    def test_one_flip_flop_sequential(self) -> None:
        text = blif(".latch d q re clk 0\n", "d clk", "q")
        got = self.run_pass(text, "--sequential")
        self.check(got, {"q": {"cc0": 3, "cc1": 3, "sc0": 1, "sc1": 1},
                         "d": {"co": 2, "so": 1}})
        comb = self.run_pass(text)  # scan view: q is a pseudo input, d a pseudo output
        self.check(comb, {"q": {"cc0": 1, "cc1": 1}, "d": {"co": 0}})
        self.assertNotIn("sc0", comb["q"])

    def test_two_bit_counter(self) -> None:
        body = (".names q0 r d0\n00 1\n.names q1 q0 r d1\n100 1\n010 1\n"
                ".latch d0 q0 re clk 0\n.latch d1 q1 re clk 0\n")
        got = self.run_pass(blif(body, "r clk", "q0 q1"), "--sequential")
        self.check(got, {"q0": {"cc0": 4, "cc1": 8, "sc0": 1, "sc1": 2}})
        self.assertEqual(self.raw["modules"][0]["converged"], 1)

    def test_wide_sop_rows_and_bound(self) -> None:
        names = " ".join(f"i{k}" for k in range(9))
        got = self.run_pass(blif(f".names {names} y\n{'1' * 9} 1\n", names, "y"))
        self.check(got, {"y": {"cc1": 10, "cc0": 10}})

    def test_summary_attributes_and_top(self) -> None:
        text = blif(".names a b y\n11 1\n.names y c z\n11 1\n", "a b c", "z")
        self.run_pass(text, "--top 2")
        top = self.raw["modules"][0]["top"]
        self.assertEqual(top["hardest_to_control"][0], {"id": top["hardest_to_control"][0]["id"],
                                                          "value": 5})
        self.assertEqual(len(top["hardest_to_observe"]), 2)

    def test_reference_equals_pass_on_small_circuits(self) -> None:
        gate = load("gate")
        text = blif(
            ".names a b c y\n11- 1\n--1 1\n.names y a w\n10 1\n01 1\n.names w b q\n00 1\n"
            ".latch q r re clk 0\n.names r a z\n1- 1\n-0 1\n",
            "a b c clk", "z w",
        )
        src = self.dir / "r.blif"
        src.write_text(text)
        for seq in (False, True):
            ok, note = gate.check_one(str(ODIN3), str(LIB), src, seq, self.dir)
            self.assertTrue(ok, note)

    def test_signal_probability_against_exact_enumeration(self) -> None:
        text = blif(".names a b y\n11 1\n.names y c z\n1- 1\n-1 1\n.names a z w\n10 1\n01 1\n",
                    "a b c", "w")
        got = self.run_pass(text, "--sigprob 20000 --seed 7 --threshold 0.3")
        ref = load("scoap_ref")
        model = ref.parse_blif(self.dir / "t.blif")["t"]
        exact: dict[str, int] = {}
        for bits in itertools.product((0, 1), repeat=3):
            env = dict(zip(model.inputs, bits))
            for ins, out, rows in model.names:
                env[out] = ref.sop_value(rows, tuple(env[x] for x in ins))
            for net, val in env.items():
                exact[net] = exact.get(net, 0) + val
        for net, ones in exact.items():
            p_exact = ones / 8
            p_sim = got[net]["p1_ppm"] / 1e6
            sigma = math.sqrt(max(p_exact * (1 - p_exact), 1e-9) / 20000)
            self.assertLess(abs(p_sim - p_exact), 5 * sigma + 1e-3, net)
        rare = set(self.raw["modules"][0]["rare"])
        names = {n["id"]: n["name"] for n in self.raw["modules"][0]["nets"]}
        self.assertEqual({names[i] for i in rare}, {"y"})  # P(y)=0.25 < 0.3; P(a)=0.5, P(z)=0.625


if __name__ == "__main__":
    unittest.main()
