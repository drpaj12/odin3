"""S1 Task 5 (spec 4): Simulation over the odin3_simulation_* ABI.

Run through CTest `python_sim`, which sets ODIN3_LIB (and preloads the sanitizer runtime for a
Debug library). Skipped where cffi is missing, unless ODIN3_REQUIRE_CFFI=1, and where ODIN3_LIB is
unset.
"""

from __future__ import annotations

import gc
import os
import time
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

try:
    import cffi  # noqa: F401

    HAVE_CFFI = True
except ImportError:
    HAVE_CFFI = False
REQUIRE_CFFI = os.environ.get("ODIN3_REQUIRE_CFFI") == "1"

if TYPE_CHECKING or HAVE_CFFI:
    from odin3 import Design, Odin3, Odin3Error

FIXTURES = Path(__file__).resolve().parents[2] / "tests" / "sim"
HIER_NAMES = [
    "clk", "a", "y", "z", "mid", "kept_r", "u0.clk", "u0.d", "u0.q", "u0.inner",
    "u1.clk", "u1.d", "u1.q", "u1.inner",
]  # fmt: skip
# Spec section 10: (rst, car_ns, car_ew) -> (light_ns, light_ew, state, timer) after the step.
TRAFFIC = [
    ((1, 0, 0), (2, 0, 0, 0)),
    ((0, 0, 1), (1, 0, 1, 1)),
    ((0, 0, 0), (1, 0, 1, 0)),
    ((0, 0, 0), (0, 2, 2, 3)),
    ((0, 0, 0), (0, 2, 2, 2)),
    ((0, 0, 0), (0, 2, 2, 1)),
    ((0, 0, 0), (0, 2, 2, 0)),
    ((0, 1, 0), (0, 1, 3, 1)),
    ((0, 0, 0), (0, 1, 3, 0)),
    ((0, 0, 0), (2, 0, 0, 3)),
]


@unittest.skipUnless(HAVE_CFFI or REQUIRE_CFFI, "cffi not installed")
@unittest.skipUnless(os.environ.get("ODIN3_LIB"), "ODIN3_LIB not set (run CTest python_sim)")
class PythonSimTest(unittest.TestCase):
    odin3: Odin3

    @classmethod
    def setUpClass(cls) -> None:
        if not HAVE_CFFI:
            raise AssertionError("ODIN3_REQUIRE_CFFI=1 but cffi is not installed")
        cls.odin3 = Odin3()
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_ERROR)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_INFO)

    def load(self, name: str) -> Design:
        design = Design(self.odin3)
        path = FIXTURES / name
        design.run_script(f'read_verilog "{path}"; proc; opt; lower')
        return design

    def test_traffic_reference_trace(self) -> None:
        with self.load("traffic.v") as design, design.simulate() as sim:
            self.assertEqual((sim["light_ns"], sim["light_ew"]), (2, 0))
            for number, (inputs, want) in enumerate(TRAFFIC, 1):
                sim["rst"], sim["car_ns"], sim["car_ew"] = inputs
                sim.step()
                got = tuple(sim[n] for n in ("light_ns", "light_ew", "state", "timer"))
                self.assertEqual(got, want, f"step {number}")
            self.assertEqual(sim.cycle, len(TRAFFIC))

    def test_signals_and_scopes(self) -> None:
        with self.load("hier.v") as design, design.simulate() as sim:
            listed = sim.signals()
            self.assertEqual([s.name for s in listed], HIER_NAMES)
            self.assertEqual([s.id for s in listed], list(range(1, 15)))
            self.assertEqual(listed[0].kind, "clock")
            self.assertEqual(listed[1].kind, "input")
            self.assertEqual((listed[1].width, listed[1].msb, listed[1].lsb), (4, 3, 0))
            self.assertEqual([s.leaf for s in sim.scopes()], ["hier", "u0", "u1"])
            self.assertEqual([s.scope for s in sim.scopes()], [0, 1, 2])
            bit = sim.signal("a[2]")
            assert bit is not None
            self.assertEqual(bit.width, 1)
            self.assertEqual(sim.signal("u0.inner"), listed[9])

    def test_unknown_name_raises_key_error(self) -> None:
        with self.load("hier.v") as design, design.simulate() as sim:
            self.assertIsNone(sim.signal("dead_r"))
            with self.assertRaisesRegex(KeyError, "dead_r: not in the simulated netlist"):
                sim["dead_r"]
            with self.assertRaisesRegex(KeyError, "nope: not in the simulated netlist"):
                sim["nope"] = 1

    def test_value_range(self) -> None:
        with self.load("traffic.v") as design, design.simulate() as sim:
            with self.assertRaises(ValueError):
                sim.set("car_ns", 2)
            with self.assertRaises(ValueError):
                sim.set("car_ns", -1)
            with self.assertRaises(Odin3Error):
                sim.set("light_ns", 1)
            with self.assertRaises(ValueError):
                sim.step(-1)
            with self.assertRaises(ValueError):
                sim.step(2**32)
            sim.step(0)
            self.assertEqual(sim.cycle, 0)

    def test_unknown_top_is_value_error(self) -> None:
        with self.load("traffic.v") as design:
            with self.assertRaises(ValueError):
                design.simulate("nope")
            with design.simulate("traffic") as sim:
                self.assertEqual(sim["light_ns"], 2)

    def test_wide_signal_roundtrip(self) -> None:
        value = (1 << 69) | 0x2AAAAAAAAAAAAAAAA
        with self.load("wide.v") as design, design.simulate() as sim:
            sim["d"] = value
            sim.step()
            self.assertEqual(sim["q"], value)
            self.assertEqual(sim["d"], value)
            with self.assertRaises(ValueError):
                sim["d"] = 1 << 70

    def test_run_pass_refused_while_open(self) -> None:
        with self.load("traffic.v") as design:
            sim = design.simulate()
            with self.assertRaises(Odin3Error):
                design.run_pass("stats")
            sim.close()
            sim.close()  # idempotent
            design.run_pass("stats")

    def test_closed_simulation(self) -> None:
        with self.load("traffic.v") as design:
            sim = design.simulate()
            sim.close()
            with self.assertRaisesRegex(RuntimeError, "simulation is closed"):
                sim.step()
            with self.assertRaisesRegex(RuntimeError, "simulation is closed"):
                sim["rst"]

    def test_design_close_closes_simulations(self) -> None:
        design = self.load("traffic.v")
        sim = design.simulate()
        design.close()
        with self.assertRaisesRegex(RuntimeError, "simulation is closed"):
            sim.step()

    def test_gc_order_is_safe(self) -> None:
        design = self.load("traffic.v")
        sim = design.simulate()
        sim.step()
        del sim
        del design
        gc.collect()
        design = self.load("traffic.v")
        sim = design.simulate()
        del design
        sim.step()
        del sim
        gc.collect()
        design = self.load("traffic.v")
        sim = design.simulate()
        box: list[object] = [design, sim]
        box.append(box)
        del design, sim, box
        gc.collect()

    def test_two_simulations_one_design(self) -> None:
        with self.load("traffic.v") as design:
            a, b = design.simulate(), design.simulate()
            for inputs, _want in TRAFFIC[:2]:
                a["rst"], a["car_ns"], a["car_ew"] = inputs
                a.step()
            self.assertEqual((a["light_ns"], a.cycle), (1, 2))
            self.assertEqual((b["light_ns"], b.cycle), (2, 0))
            b.close()
            a.close()

    def test_reset_and_cycle(self) -> None:
        with self.load("traffic.v") as design, design.simulate() as sim:
            sim["car_ew"] = 1
            sim.step(3)
            self.assertEqual(sim.cycle, 3)
            self.assertNotEqual(sim["light_ns"], 2)
            sim.reset()
            self.assertEqual(sim.cycle, 0)
            self.assertEqual(sim["light_ns"], 2)

    def test_steps_fast_enough(self) -> None:
        with self.load("traffic.v") as design, design.simulate() as sim:
            start = time.perf_counter()
            for i in range(600):
                sim["car_ns"] = i % 2
                sim["car_ew"] = 1 - i % 2
                sim.step()
                sim["light_ns"], sim["light_ew"]  # noqa: B018
            self.assertLess(time.perf_counter() - start, 10.0)


if __name__ == "__main__":
    unittest.main()
