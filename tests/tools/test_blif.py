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
            "unknown directive": ".model t\n.inputs a\n.gate and2 A=a\n.end\n",
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


if __name__ == "__main__":
    unittest.main()
