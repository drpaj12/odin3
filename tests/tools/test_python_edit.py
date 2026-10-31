"""Netlist editing through the Python binding (spec docs/specs/2026-10-31-netlist-edit-sim-design.md
sections 2, 3 and 7): add net and $sop, connect and rewire, set_cover, remove, edit_tag, the
BLIF round trip and the missing-symbol error.

Run through CTest `python_edit`, which sets ODIN3_LIB. Skipped where cffi is missing, unless
ODIN3_REQUIRE_CFFI=1.
"""

from __future__ import annotations

import os
import tempfile
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
    import odin3
    from odin3 import Design

BLIF_TEXT = ".model m\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n"


@unittest.skipUnless(HAVE_CFFI or REQUIRE_CFFI, "cffi not installed")
@unittest.skipUnless(os.environ.get("ODIN3_LIB"), "ODIN3_LIB not set (run CTest python_edit)")
class PythonEditTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "in.blif"
        self.path.write_text(BLIF_TEXT)
        self.design = Design()
        self.addCleanup(self.design.close)
        self.design.run_pass("read_blif", str(self.path))
        top = self.design.top()
        assert top is not None
        self.top = top

    def test_insert_gate_between_nets(self) -> None:
        a, b = self.top.net("a"), self.top.net("b")
        assert a is not None and b is not None
        out = self.top.add_net("t_q", "trojan:t1")
        gate = self.top.add_sop(2, "10 1\n", "t_g", "trojan:t1")
        pins = gate.pins()
        pins[0].connect(a)
        pins[1].connect(b)
        pins[2].connect(out)
        pins[1].rewire(a)
        net = pins[1].net()
        assert net is not None
        self.assertEqual(net.name, "a")
        self.assertEqual(out.edit_tag, "trojan:t1")
        self.assertEqual(gate.edit_tag, "trojan:t1")
        self.assertIsNone(a.edit_tag)

    def test_set_cover_returns_the_twin(self) -> None:
        gate = self.top.add_sop(2, "11 1\n", "g", "t")
        a = self.top.net("a")
        assert a is not None
        gate.pins()[0].connect(a)
        twin = gate.set_cover("00 1\n", "t2")
        self.assertFalse(gate.is_live)
        self.assertEqual(twin.name, "g")
        self.assertEqual(twin.edit_tag, "t2")
        pin_net = twin.pins()[0].net()
        assert pin_net is not None
        self.assertEqual(pin_net.name, "a")

    def test_twin_keeps_source_location_attrs_and_param_twin(self) -> None:
        y = self.top.net("y")
        assert y is not None
        driver = y.pins()[0].node()
        before = [(src.file, src.line) for src in driver.sources()]
        self.assertTrue(before)
        driver.set_attr("note", "kept")
        twin = driver.set_cover("00 1\n", "trojan:t9")
        self.assertEqual([(src.file, src.line) for src in twin.sources()], before)
        self.assertEqual(twin.attr("note"), "kept")
        self.assertEqual(twin.edit_tag, "trojan:t9")
        self.assertGreaterEqual(len(twin.provenance()), 2)
        gate = self.top.add_sop(2, "11 1\n", "pg", "t")
        again = gate.set_param_int(0, 2, "t5")
        self.assertFalse(gate.is_live)
        self.assertTrue(again.is_live)

    def test_remove_driver_needs_the_flag(self) -> None:
        y = self.top.net("y")
        assert y is not None
        driver = y.pins()[0].node()
        with self.assertRaises(odin3.Odin3Error):
            driver.remove()
        self.assertTrue(driver.is_live)

    def test_refusals_raise(self) -> None:
        self.top.add_net("n1", "t")
        with self.assertRaises(odin3.Odin3Error):
            self.top.add_net("n1", "t")
        with self.assertRaises(odin3.Odin3Error):
            self.top.add_net("n2", "")
        with self.assertRaises(odin3.Odin3Error):
            self.top.add_node("$nosuch", "x", "t")

    def test_remove_and_round_trip(self) -> None:
        net = self.top.add_net("tmp", "t")
        gate = self.top.add_sop(1, "1 1\n", "tmpg", "t")
        gate.remove()
        net.remove()
        out = Path(self.tmp.name) / "out.blif"
        self.design.run_pass("write_blif", str(out))
        again = Design()
        self.addCleanup(again.close)
        again.run_pass("read_blif", str(out))
        top = again.top()
        assert top is not None
        self.assertEqual(top.node_count(), self.top.node_count())

    def test_sequences_and_toggles(self) -> None:
        seq = Path(self.tmp.name) / "seq.blif"
        seq.write_text(
            ".model s\n.inputs clk en\n.outputs q\n.names en q d\n10 1\n01 1\n"
            ".latch d q re clk 0\n.end\n"
        )
        design = Design()
        self.addCleanup(design.close)
        design.run_pass("read_blif", str(seq))
        sim = design.simulate()
        self.assertIsNotNone(sim.signal("d"))  # an internal net, no (* keep *)
        sim.set_toggle_counting()
        rows = sim.run_vectors(["en"], [[1], [1], [0], [1]], ["q", "d"])
        self.assertEqual(rows, [[1, 0], [0, 1], [0, 0], [1, 0]])
        self.assertEqual(sim.toggles("q"), 3)
        self.assertEqual(sim.toggles("d"), 2)
        sim.reset()
        self.assertEqual(sim.toggles("q"), 0)
        sim.close()

    def test_toggles_of_an_untracked_signal_explains_itself(self) -> None:
        sim = self.design.simulate()
        self.addCleanup(sim.close)
        sim.set_toggle_counting()
        late = sim.signal("y")
        assert late is not None
        with self.assertRaises(odin3.Odin3Error) as ctx:
            sim.toggles(late._replace(id=10**6))
        self.assertIn("set_toggle_counting", str(ctx.exception))

    def test_missing_symbol_is_a_clear_error(self) -> None:
        class OldLib:
            pass

        class OldOdin3:
            lib = OldLib()

        class OldDesign:
            odin3 = OldOdin3()

        with self.assertRaises(odin3.Odin3Error) as ctx:
            odin3._edit_probe(OldDesign(), "odin3_edit_add_net")  # type: ignore[arg-type]
        self.assertIn("libodin3 too old", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
