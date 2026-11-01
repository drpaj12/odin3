"""Netlist editing through the Python binding (spec docs/specs/2026-10-31-netlist-edit-sim-design.md
sections 2, 3 and 7): add net and $sop, connect and rewire, set_cover, remove, edit_tag, the
BLIF round trip and the missing-symbol error.

Run through CTest `python_edit`, which sets ODIN3_LIB. Skipped where cffi is missing, unless
ODIN3_REQUIRE_CFFI=1.
"""

from __future__ import annotations

import os
import tempfile
import threading
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

    def test_events_for_rewire_disconnect_and_twin(self) -> None:
        a, b = self.top.net("a"), self.top.net("b")
        assert a is not None and b is not None
        gate = self.top.add_sop(2, "11 1\n", "ev", "trojan:t1")
        self.assertEqual(gate.edit_events, [("added", "trojan:t1")])
        pin = gate.pins()[0]
        pin.connect(a)
        pin.rewire(b)
        pin.rewire(a, tag="r1")
        pin.disconnect(tag="d1")
        self.assertEqual(
            gate.edit_events,
            [("added", "trojan:t1"), ("rewired", "edit"), ("rewired", "r1"), ("removed", "d1")],
        )
        twin = gate.set_cover("00 1\n", "t2")
        self.assertEqual(twin.edit_events[-1], ("param", "t2"))
        self.assertEqual(twin.edit_events[0], ("added", "trojan:t1"))
        self.assertEqual(a.edit_events, [])
        with self.assertRaises(odin3.Odin3Error):
            pin.rewire(a, tag="")

    def test_events_survive_blif_and_json_export(self) -> None:
        net = self.top.add_net("tq", "trojan:t1")
        gate = self.top.add_sop(2, "11 1\n", "tg", "trojan:t1")
        gate.pins()[2].connect(net)
        blif = Path(self.tmp.name) / "tags.blif"
        plain = Path(self.tmp.name) / "plain.blif"
        js = Path(self.tmp.name) / "tags.json"
        self.design.run_pass("write_blif", f"--edit-tags {blif}")
        self.design.run_pass("write_blif", str(plain))
        self.design.run_pass("write_json", f"--odin3 {js}")
        self.assertIn(".attr odin3_edit added:trojan:t1", blif.read_text())
        self.assertNotIn("odin3_edit", plain.read_text())
        for kind, path in (("read_blif", blif), ("read_json", js)):
            again = Design()
            self.addCleanup(again.close)
            again.run_pass(kind, str(path))
            top = again.top()
            assert top is not None
            node = top.node("tg")
            assert node is not None
            self.assertEqual(node.edit_events, [("added", "trojan:t1")], kind)
            self.assertEqual(node.edit_tag, "trojan:t1", kind)

    def test_add_node_with_initial_params(self) -> None:
        plain = self.top.add_node("$_DFF_P_", "ff_a", "t")
        zero = self.top.add_node("$_DFF_P_", "ff_b", "t", params={"INIT": 0})
        self.assertEqual(plain.params()["INIT"], 3)
        self.assertEqual(zero.params()["INIT"], 0)
        self.assertTrue(zero.is_live)
        with self.assertRaises(odin3.Odin3Error):
            self.top.add_node("$_DFF_P_", "ff_c", "t", params={"NOPE": 1})
        self.assertIsNone(self.top.node("ff_c"))

    def test_top_before_read_verilog(self) -> None:
        src = Path(self.tmp.name) / "two.v"
        src.write_text(
            "module one(input a, output y); assign y = a; endmodule\n"
            "module two(input a, output y); assign y = ~a; endmodule\n"
        )
        for want in ("one", "two"):
            design = Design(top=want)
            self.addCleanup(design.close)
            design.run_script(f"read_verilog {src}")
            top = design.top()
            assert top is not None
            self.assertEqual(top.name, want)
        other = Design()
        self.addCleanup(other.close)
        other.run_script(f"read_verilog {src}", top="two")
        top = other.top()
        assert top is not None
        self.assertEqual(top.name, "two")
        # the name is not left set for the next, top-less design: two roots are ambiguous again
        third = Design()
        self.addCleanup(third.close)
        with self.assertRaises(odin3.Odin3Error):
            third.run_script(f"read_verilog {src}")

    def test_tops_of_concurrent_threads_do_not_mix(self) -> None:
        src = Path(self.tmp.name) / "two.v"
        src.write_text(
            "module one(input a, output y); assign y = a; endmodule\n"
            "module two(input a, output y); assign y = ~a; endmodule\n"
        )
        errors: list[str] = []

        def work(want: str) -> None:
            for _ in range(15):
                design = Design(top=want)
                try:
                    design.run_script(f"read_verilog {src}")
                    top = design.top()
                    if top is None or top.name != want:
                        errors.append(f"{want}: got {None if top is None else top.name}")
                except odin3.Odin3Error as err:
                    errors.append(f"{want}: {err}")
                finally:
                    design.close()

        threads = [threading.Thread(target=work, args=(name,)) for name in ("one", "two")]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(errors, [])
        # and nothing is left set afterwards
        third = Design()
        self.addCleanup(third.close)
        with self.assertRaises(odin3.Odin3Error):
            third.run_script(f"read_verilog {src}")

    def test_params_need_a_library_that_has_them(self) -> None:
        # a library without odin3_edit_get_events predates the parameter fields: refuse, as it
        # would otherwise ignore them
        real = self.design.odin3.lib

        class NoEvents:
            def __getattr__(self, name: str) -> object:
                if name == "odin3_edit_get_events":
                    raise AttributeError(name)
                return getattr(real, name)

        self.design.odin3.lib = NoEvents()
        try:
            with self.assertRaises(odin3.Odin3Error) as ctx:
                self.top.add_node("$_DFF_P_", "ff_old", "t", params={"INIT": 0})
            self.assertIn("libodin3 too old", str(ctx.exception))
        finally:
            self.design.odin3.lib = real

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
