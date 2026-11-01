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
INF = 10**18  # unreachable (SCOAP_INF)
SAT = 10**15  # finite values saturate here (SCOAP_SAT)


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

    def check_both(self, text: str, want: dict[str, dict[str, int]], flags: str = "") -> None:
        """Hand-derived numbers must come out of the pass AND of the Python reference."""
        got = self.run_pass(text, flags)
        self.check(got, want)
        ref = load("scoap_ref").reference(self.dir / "t.blif", "--sequential" in flags)
        self.check(ref["t"], want)

    def check(self, nets: dict[str, dict[str, int]], want: dict[str, dict[str, int]]) -> None:
        for name, vals in want.items():
            for key, val in vals.items():
                self.assertEqual(nets[name][key], val, f"{name}.{key}")

    def test_and_chain(self) -> None:
        text = blif(".names a b y\n11 1\n.names y c z\n11 1\n", "a b c", "z")
        got = self.run_pass(text)
        self.check(
            got,
            {
                "a": {"cc0": 1, "cc1": 1, "co": 4},
                "c": {"co": 4},
                "y": {"cc0": 2, "cc1": 3, "co": 2},
                "z": {"cc0": 2, "cc1": 5, "co": 0},
            },
        )

    def test_or_not_xor(self) -> None:
        text = blif(
            ".names a b o\n1- 1\n-1 1\n.names a n\n0 1\n.names a b x\n10 1\n01 1\n",
            "a b",
            "o n x",
        )
        got = self.run_pass(text)
        self.check(
            got,
            {
                "o": {"cc0": 3, "cc1": 2},
                "n": {"cc0": 2, "cc1": 2},
                "x": {"cc0": 3, "cc1": 3},
                "a": {"co": 1},  # the NOT is cheapest
            },
        )

    def test_observability_values(self) -> None:
        # co(a) through OR (b to 0: 1, +1) = 2; through XOR (min(cc0 b, cc1 b) = 1, +1) = 2
        got = self.run_pass(blif(".names a b o\n1- 1\n-1 1\n", "a b", "o"))
        self.check(got, {"a": {"co": 2}, "b": {"co": 2}})
        got = self.run_pass(blif(".names a b x\n10 1\n01 1\n", "a b", "x"))
        self.check(got, {"a": {"co": 2}, "b": {"co": 2}})

    def test_mux(self) -> None:
        # y = s ? b : a, primes of y=1: (s0,a1) (s1,b1) (a1,b1) -> cc1 = 1 + 2 = 3.
        # Textbook CO of a data input = CO(y) + 1 + CC(select that picks it), b is a don't care:
        # co(a) = 0 + 1 + cc0(s) = 2, co(b) = 0 + 1 + cc1(s) = 2. co(s): a != b with a known
        # polarity, (a=0,b=1) costs 2, so co(s) = 0 + 1 + 2 = 3.
        text = blif(".names s a b y\n01- 1\n1-1 1\n", "s a b", "y")
        self.check_both(
            text, {"y": {"cc0": 3, "cc1": 3}, "a": {"co": 2}, "b": {"co": 2}, "s": {"co": 3}}
        )

    def test_sop_off_set_cover(self) -> None:
        # OFF-set cover of a NAND: rows give 0 on a hit; y = NOT(a AND b): cc0 = 3, cc1 = 2
        got = self.run_pass(blif(".names a b y\n11 0\n", "a b", "y"))
        self.check(got, {"y": {"cc0": 3, "cc1": 2}})

    def test_constants_and_unreachable(self) -> None:
        text = blif(
            ".names one\n1\n.names a one y\n11 1\n.names zero\n.names zero a q\n11 1\n", "a", "y q"
        )
        got = self.run_pass(text)
        self.check(
            got, {"one": {"cc0": INF, "cc1": 1}, "y": {"cc0": 2, "cc1": 3}, "q": {"cc1": INF}}
        )
        self.assertGreaterEqual(self.log.count("unreachable"), 1)

    def test_one_flip_flop_sequential(self) -> None:
        text = blif(".latch d q re clk 0\n", "d clk", "q")
        got = self.run_pass(text, "--sequential")
        self.check(got, {"q": {"cc0": 3, "cc1": 3, "sc0": 1, "sc1": 1}, "d": {"co": 2, "so": 1}})
        comb = self.run_pass(text)  # scan view: q is a pseudo input, d a pseudo output
        self.check(comb, {"q": {"cc0": 1, "cc1": 1}, "d": {"co": 0}})
        self.assertNotIn("sc0", comb["q"])

    def test_two_bit_counter(self) -> None:
        body = (
            ".names q0 r d0\n00 1\n.names q1 q0 r d1\n100 1\n010 1\n"
            ".latch d0 q0 re clk 0\n.latch d1 q1 re clk 0\n"
        )
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
        self.assertEqual(
            top["hardest_to_control"][0], {"id": top["hardest_to_control"][0]["id"], "value": 5}
        )
        self.assertEqual(len(top["hardest_to_observe"]), 2)

    def test_gate_cells_equal_names_form(self) -> None:
        text = (
            ".model t\n.inputs a b s\n.outputs y z m\n.subckt $_AND_ A=a B=b Y=y\n"
            ".subckt $_XOR_ A=a B=b Y=z\n.subckt $_MUX_ A=a B=b S=s Y=m\n.end\n"
        )
        src = self.dir / "g.blif"
        src.write_text(text)
        out = self.dir / "g.json"
        done = subprocess.run(
            [str(ODIN3), "-p", f"read_blif {src}; scoap --out {out}"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        nets = {n["name"]: n for n in json.loads(out.read_text())["modules"][0]["nets"]}
        self.check(
            nets,
            {
                "y": {"cc0": 2, "cc1": 3},
                "z": {"cc0": 3, "cc1": 3},
                "m": {"cc0": 3, "cc1": 3, "co": 0},
                "s": {"co": 3},
                "a": {"co": 2},
            },
        )
        self.assertEqual(len(nets), 6)

    def test_lut_mapped_equals_names_form(self) -> None:
        text = blif(".names a b y\n11 1\n", "a b", "y")
        src = self.dir / "l.blif"
        src.write_text(text)
        out = self.dir / "l.json"
        script = f"read_techlib {LIB}; read_blif {src}; abc --lut 4; scoap --out {out}"
        done = subprocess.run([str(ODIN3), "-p", script], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        nets = {n["name"]: n for n in json.loads(out.read_text())["modules"][0]["nets"]}
        self.check(nets, {"y": {"cc0": 2, "cc1": 3}, "a": {"co": 2}})

    def test_black_box_is_opaque_and_named(self) -> None:
        text = (
            ".model t\n.inputs a\n.outputs y\n.subckt box i=a o=y\n.end\n"
            ".model box\n.inputs i\n.outputs o\n.blackbox\n.end\n"
        )
        got = self.run_pass(text)
        self.check(got, {"y": {"cc0": 1, "cc1": 1, "co": 0}, "a": {"co": 0}})
        self.assertIn("opaque cell type box", self.log)

    def test_reference_equals_pass_on_small_circuits(self) -> None:
        gate = load("gate")
        text = blif(
            ".names a b c y\n11- 1\n--1 1\n.names y a w\n10 1\n01 1\n.names w b q\n00 1\n"
            ".latch q r re clk 0\n.names r a z\n1- 1\n-0 1\n",
            "a b c clk",
            "z w",
        )
        src = self.dir / "r.blif"
        src.write_text(text)
        for seq in (False, True):
            ok, note = gate.check_one(str(ODIN3), str(LIB), src, seq, self.dir)
            self.assertTrue(ok, note)

    def test_signal_probability_against_exact_enumeration(self) -> None:
        text = blif(
            ".names a b y\n11 1\n.names y c z\n1- 1\n-1 1\n.names a z w\n10 1\n01 1\n", "a b c", "w"
        )
        got = self.run_pass(text, "--sigprob 20000 --seed 7 --threshold 0.3")
        ref = load("scoap_ref")
        model = ref.parse_blif(self.dir / "t.blif")["t"]
        exact: dict[str, int] = {}
        for bits in itertools.product((0, 1), repeat=3):
            env = dict(zip(model.inputs, bits, strict=True))
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

    def test_empty_cover_is_constant_zero(self) -> None:
        # No rows: y = 0. cc0 = (empty cube, cost 0) + 1 = 1, cc1 unreachable, nothing sensitises.
        text = blif(".names a b y\n", "a b", "y")
        self.check_both(text, {"y": {"cc0": 1, "cc1": INF}, "a": {"co": INF}, "b": {"co": INF}})
        # With a live AND before it: z = a AND b gives co(a) = co(z) + 1 + cc1(b) = 2.
        text = blif(".names a b z\n11 1\n.names a b y\n", "a b", "z y")
        self.check_both(
            text, {"z": {"cc0": 2, "cc1": 3}, "y": {"cc0": 1, "cc1": INF}, "a": {"co": 2}}
        )

    def test_constant_cell_after_its_reader(self) -> None:
        # one = 1 (cc1 1, cc0 INF) defined after y = a AND one: cc0(y) = cc0(a) + 1 = 2,
        # cc1(y) = 1 + 1 + 1 = 3, co(a) = 0 + 1 + cc1(one) = 2, co(one) = 0 + 1 + cc1(a) = 2.
        text = blif(".names a one y\n11 1\n.names one\n1\n", "a", "y")
        self.check_both(
            text, {"one": {"cc0": INF, "cc1": 1}, "y": {"cc0": 2, "cc1": 3}, "a": {"co": 2}}
        )

    def test_loop_needs_more_than_nets_plus_2_sweeps(self) -> None:
        # Ring x1 = a AND x6, x2 = NOT x1, x3..x6 = BUF chain, cells listed in reverse. Going
        # round once flips the polarity, so cc1(x1) needs cc1(x6), which needs cc1(x2) = cc0(x1)+1.
        # cc0(x1) = min(1, ..) + 1 = 2; cc1(x2) = 3, x3 = 4, x4 = 5, x5 = 6, x6 = 7;
        # cc1(x1) = 1 + 7 + 1 = 9; cc0(x2) = 10, x3 = 11, x4 = 12, x5 = 13, x6 = 14.
        rows = [".names a x6 x1\n11 1\n", ".names x1 x2\n0 1\n"]
        rows += [f".names x{k - 1} x{k}\n1 1\n" for k in range(3, 7)]
        text = blif("".join(reversed(rows)), "a", "x6")
        want = {"x1": (2, 9), "x2": (10, 3), "x3": (11, 4), "x4": (12, 5), "x5": (13, 6)}
        want_nets = {k: {"cc0": v[0], "cc1": v[1]} for k, v in want.items()}
        want_nets["x6"] = {"cc0": 14, "cc1": 7}
        self.check_both(text, want_nets)
        self.assertEqual(self.raw["modules"][0]["converged"], 1)

    def chain(self, count: int, reverse: bool) -> str:
        rows = [f".names x{k - 1} x{k}\n1 1\n" for k in range(1, count + 1)]
        return blif("".join(reversed(rows) if reverse else rows), "x0", f"x{count}")

    def test_reverse_chain_is_linear_work(self) -> None:
        # x_k = BUF(x_{k-1}): cc1(x_k) = k + 1, co(x_k) = N - k. Stored in reverse node order the
        # worklist does O(N) cell evaluations (full sweeps did N sweeps of N cells).
        count = 2000
        rev = self.run_pass(self.chain(count, True))
        evals = self.raw["modules"][0]["evals"]
        self.check(rev, {"x0": {"co": count}, f"x{count}": {"cc1": count + 1, "co": 0}})
        self.assertLessEqual(evals, 10 * count)
        fwd = self.run_pass(self.chain(count, False))
        keys = ("cc0", "cc1", "co")
        self.assertEqual(
            {k: [v[x] for x in keys] for k, v in rev.items()},
            {k: [v[x] for x in keys] for k, v in fwd.items()},
        )

    def test_deep_reconvergence_saturates_not_inf(self) -> None:
        # s_k = s_{k-1} AND t_{k-1}, t = BUF(s_{k-1}): cc1(s_k) = c + (c + 1) + 1 = 2c + 2, so
        # cc1(s_k) = 3 * 2^k - 2 (cc1(s_0) = 1); cc0(s_k) = cc0(s_{k-1}) + 1 = k + 1. s_40 is
        # 3298534883326, below the cap; from k = 49 on the value is the cap SAT = 10^15.
        levels = 60
        rows = ""
        for k in range(1, levels + 1):
            rows += f".names s{k - 1} t{k - 1}\n1 1\n.names s{k - 1} t{k - 1} s{k}\n11 1\n"
        text = blif(rows, "s0", f"s{levels}")
        want = {
            "s40": {"cc0": 41, "cc1": 3 * 2**40 - 2},
            "s48": {"cc1": 3 * 2**48 - 2},
            "s49": {"cc1": SAT},
            "s60": {"cc0": 61, "cc1": SAT},
        }
        self.check_both(text, want)
        mod = self.raw["modules"][0]
        self.assertEqual(mod["unreachable"], 0)
        self.assertGreater(mod["saturated"], 0)
        self.assertEqual(self.raw["sat"], SAT)
        for net in mod["nets"]:
            for key in ("cc0", "cc1", "co"):
                self.assertLess(net[key], INF, f"{net['name']}.{key}")

    def test_duplicate_input_nets_collapse(self) -> None:
        # XOR(a, a) = 0 and AND(a, NOT a) = 0: constant 0, cc0 = 1 (empty cube + 1), cc1 INF.
        for rows in ("10 1\n01 1\n", "10 1\n"):
            text = blif(f".names a a y\n{rows}", "a", "y")
            self.check_both(text, {"y": {"cc0": 1, "cc1": INF}, "a": {"co": INF}})
        # AND(a, a) = a: the shared net counts once, cc0 = cc1 = 1 + 1 = 2, co(a) = 0 + 1 = 1.
        text = blif(".names a a y\n11 1\n", "a", "y")
        self.check_both(text, {"y": {"cc0": 2, "cc1": 2}, "a": {"co": 1}})

    def test_reference_equals_pass_on_edge_cases(self) -> None:
        gate = load("gate")
        cases = [
            ".names a b y\n",
            ".names a a y\n10 1\n01 1\n.names a b t\n11 1\n",
            ".names s a b y\n01- 1\n1-1 1\n.names y s z\n10 1\n01 1\n",
        ]
        for k, rows in enumerate(cases):
            src = self.dir / f"e{k}.blif"
            src.write_text(blif(rows, "a b s", "y t z"))
            for seq in (False, True):
                ok, note = gate.check_one(str(ODIN3), str(LIB), src, seq, self.dir)
                self.assertTrue(ok, note)

    def test_netlist_hash_and_version_stamp(self) -> None:
        text = blif(".names a b y\n11 1\n", "a b", "y")
        self.run_pass(text)
        first = self.raw["modules"][0]["netlist_hash"]
        self.assertEqual(self.raw["version"], 2)
        self.run_pass(text)
        self.assertEqual(self.raw["modules"][0]["netlist_hash"], first)
        self.run_pass(blif(".names a b y\n1- 1\n-1 1\n", "a b", "y"))
        self.assertNotEqual(self.raw["modules"][0]["netlist_hash"], first)

    def test_top_rejects_values_above_uint32(self) -> None:
        src = self.dir / "t.blif"
        src.write_text(blif(".names a b y\n11 1\n", "a b", "y"))
        for value, code in (("4294967295", 0), ("4294967296", 1)):
            script = f"read_techlib {LIB}; read_blif {src}; scoap --top {value}"
            done = subprocess.run([str(ODIN3), "-p", script], capture_output=True, text=True)
            self.assertEqual(done.returncode == 0, code == 0, done.stderr)

    def test_constant_nets_are_not_rare(self) -> None:
        text = blif(".names one\n1\n.names a one y\n11 1\n", "a", "y")
        got = self.run_pass(text, "--sigprob 2000 --threshold 0.3")
        self.assertEqual(got["one"]["p1_ppm"], 1000000)
        rare = {n["name"] for n in got.values() if n["id"] in self.raw["modules"][0]["rare"]}
        self.assertNotIn("one", rare)

    def test_sigprob_skipped_when_simulator_refuses_a_latch_hold_loop(self) -> None:
        # d = latch(D = mux(en2 ? x : q)): D reads the latch's own Q (an inferred hold path), a
        # structural loop through a transparent latch that the simulator refuses. SCOAP still
        # runs; p1_ppm is -1 on every net and the log says why.
        rows = (
            ".subckt $_MUX_ A=q B=x S=sel Y=dd\n"
            ".latch dd q al en 3\n"
            ".names q y\n1 1\n"
        )
        got = self.run_pass(blif(rows, "x sel en", "y"), "--sigprob 100")
        self.assertIn("--sigprob skipped", self.log)
        self.assertEqual({n["p1_ppm"] for n in got.values()}, {-1})
        self.assertIn("cc0", got["q"])


if __name__ == "__main__":
    unittest.main()
