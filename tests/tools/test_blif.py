"""Tests for the shared BLIF reader (tools/blif/blif.py)."""

from __future__ import annotations

import unittest

import blif

from . import helpers


class ParseTest(unittest.TestCase):
    def test_continuation_and_comments(self) -> None:
        text = (
            "# header comment\n"
            ".model top  # trailing comment\n"
            ".inputs a \\\n"
            "  b\\\n"
            " c\n"
            ".outputs y\n"
            ".names a b \\\n c y\n"
            "111 1\n"
            ".end\n"
        )
        top = blif.parse_text(text).top
        self.assertEqual(top.inputs, ["a", "b", "c"])
        cell = top.cells[0]
        assert isinstance(cell, blif.Names)
        self.assertEqual(cell.inputs, ["a", "b", "c"])
        self.assertEqual(cell.rows, [("111", "1")])

    def test_vtr_constructs(self) -> None:
        netlist = blif.parse_file(helpers.fixture("blink.odin.blif"))
        self.assertEqual(netlist.top.name, "blink")
        self.assertEqual([m.name for m in netlist.models], ["blink", "adder"])
        self.assertTrue(netlist.models[1].blackbox)
        latches = [c for c in netlist.top.cells if isinstance(c, blif.Latch)]
        self.assertEqual(len(latches), 5)
        self.assertEqual((latches[0].ltype, latches[0].control, latches[0].init),
                         ("re", "blink^clk", "3"))
        subckts = [c for c in netlist.top.cells if isinstance(c, blif.Subckt)]
        self.assertIn(("cin[0]", "blink^ADD~2-0[0]"), subckts[0].conns)

    def test_attrs_attach_to_cell(self) -> None:
        top = blif.parse_file(helpers.fixture("comb_parmys.blif")).top
        attrs = [a for c in top.cells for a in c.attrs]
        self.assertIn((".attr", 'src "comb.v:5.12-5.19"'), attrs)
        self.assertEqual(sum(1 for k, _ in attrs if k == ".cname"), 1)

    def test_malformed(self) -> None:
        cases = {
            "row width": ".model t\n.inputs a\n.outputs y\n.names a y\n11 1\n.end\n",
            "row outside names": ".model t\n.inputs a\n.outputs y\n1 1\n.end\n",
            "bad init": ".model t\n.inputs a c\n.outputs y\n.latch a y re c 7\n.end\n",
            "bad type": ".model t\n.inputs a c\n.outputs y\n.latch a y xx c 0\n.end\n",
            "two drivers": ".model t\n.inputs a\n.outputs y\n.names a y\n1 1\n.names y\n.end\n",
            "undefined model": ".model t\n.inputs a\n.outputs y\n.subckt foo x=a z=y\n.end\n",
            "unknown port": (".model t\n.inputs a\n.outputs y\n.subckt s q=a\n.end\n"
                             ".model s\n.inputs x\n.blackbox\n.end\n"),
            "unknown directive": ".model t\n.inputs a\n.mystery and2 A=a\n.end\n",
            "gate not enabled": ".model t\n.inputs a\n.gate and2 A=a\n.end\n",
            "mixed cover": ".model t\n.inputs a b\n.outputs y\n.names a b y\n11 1\n00 0\n.end\n",
            "no model": "# nothing\n",
            "cell outside model": ".names a y\n1 1\n",
            "attr without cell": ".model t\n.attr src x\n.end\n",
        }
        for name, text in cases.items():
            with self.subTest(name), self.assertRaises(blif.BlifError):
                blif.parse_text(text, "t.blif")

    def test_error_has_location(self) -> None:
        with self.assertRaises(blif.BlifError) as ctx:
            blif.parse_text(".model t\n.inputs a\n.outputs y\n.names a y\n11 1\n", "x.blif")
        self.assertTrue(str(ctx.exception).startswith("x.blif:5: "))


class GateTest(unittest.TestCase):
    """``.gate`` is Yosys's spelling of a cell instance; parsed like ``.subckt`` on request."""

    TEXT = (".model t\n.inputs clk a b\n.outputs y\n"
            ".gate single_port_ram addr[0]=a clk=clk data[0]=b out[0]=q we=a\n"
            ".cname ram0\n"
            ".names q y\n1 1\n.end\n")

    def test_gate_is_off_by_default(self) -> None:
        with self.assertRaises(blif.BlifError) as ctx:
            blif.parse_text(self.TEXT, "g.blif")
        self.assertIn("g.blif:4: unsupported directive .gate", str(ctx.exception))

    def test_gate_parses_like_subckt(self) -> None:
        cell = blif.parse_text(self.TEXT, "g.blif", gates=True).top.cells[0]
        assert isinstance(cell, blif.Subckt)
        self.assertTrue(cell.gate)
        self.assertEqual(cell.model, "single_port_ram")
        self.assertEqual(cell.conns, [("addr[0]", "a"), ("clk", "clk"), ("data[0]", "b"),
                                      ("out[0]", "q"), ("we", "a")])
        self.assertEqual(cell.attrs, [(".cname", "ram0")])
        self.assertEqual(cell.line, 4)

    def test_subckt_is_not_a_gate(self) -> None:
        text = (".model t\n.inputs a\n.outputs y\n.subckt s x=a z=y\n.end\n"
                ".model s\n.inputs x\n.outputs z\n.blackbox\n.end\n")
        cell = blif.parse_text(text, gates=True).top.cells[0]
        assert isinstance(cell, blif.Subckt)
        self.assertFalse(cell.gate)

    def test_undefined_model_is_still_an_error_for_subckt(self) -> None:
        text = ".model t\n.inputs a\n.outputs y\n.subckt foo x=a z=y\n.end\n"
        with self.assertRaises(blif.BlifError):
            blif.parse_text(text, gates=True)

    def test_gate_outputs_by_cell_type(self) -> None:
        cases = {
            ("single_port_ram", "out[3]"): True,
            ("single_port_ram", "data[3]"): False,
            ("dual_port_ram", "out1[0]"): True,
            ("dual_port_ram", "out2"): True,
            ("dual_port_ram", "addr1[0]"): False,
            ("multiply", "out[35]"): True,
            ("multiply", "a[0]"): False,
            ("$_DFF_P_", "Q"): True,
            ("$_DFF_P_", "D"): False,
        }
        for (cell_type, formal), expected in cases.items():
            with self.subTest(cell_type=cell_type, formal=formal):
                self.assertEqual(blif.gate_port_is_output(cell_type, formal), expected)

    # An inout port is in .inputs and .outputs (Yosys's write_blif, ours): the outside drives it and
    # the model may drive it too, once. Ordinary nets keep the one-driver rule (2H).
    INOUT = (".model t\n.inputs a en io\n.outputs io\n"
             ".names a en io\n11 1\n.end\n")

    def test_inout_may_have_one_internal_driver(self) -> None:
        blif.parse_text(self.INOUT, "io.blif")
        blif.parse_text(".model t\n.inputs a io\n.outputs io\n.names a io\n1 1\n.end\n")

    def test_inout_second_internal_driver_is_an_error(self) -> None:
        text = self.INOUT.replace(".end\n", ".names a io\n1 1\n.end\n")
        with self.assertRaises(blif.BlifError) as ctx:
            blif.parse_text(text, "io.blif")
        self.assertIn("more than one driver", str(ctx.exception))

    def test_plain_input_driven_internally_is_still_an_error(self) -> None:
        text = ".model t\n.inputs a en\n.outputs y\n.names a en\n1 1\n.names a y\n1 1\n.end\n"
        with self.assertRaises(blif.BlifError) as ctx:
            blif.parse_text(text, "in.blif")
        self.assertIn("more than one driver", str(ctx.exception))

    def test_output_only_net_is_not_an_inout(self) -> None:
        text = ".model t\n.inputs a\n.outputs y\n.names a y\n1 1\n.names a y\n0 1\n.end\n"
        with self.assertRaises(blif.BlifError):
            blif.parse_text(text, "o.blif")

    def test_gate_drivers_are_checked(self) -> None:
        text = (".model t\n.inputs a\n.outputs y\n"
                ".gate single_port_ram clk=a out[0]=y\n.names a y\n1 1\n.end\n")
        with self.assertRaises(blif.BlifError) as ctx:
            blif.parse_text(text, "g.blif", gates=True)
        self.assertIn("more than one driver", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
