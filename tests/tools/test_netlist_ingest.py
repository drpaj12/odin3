"""Tests for tools/netlist-ingest (Phase 6.1): the name extraction from a Verilog netlist and a
BLIF, and the gates failing on a planted difference (a NAND2 turned into a NOR2 in a copy of the
fixtures). The planted-difference tests need ``ODIN3_CLI`` (CTest python_tools)."""

from __future__ import annotations

import argparse
import os
import shutil
import unittest
from pathlib import Path
from unittest import mock

import netlist_ingest as ni

from .helpers import REPO_ROOT, scratch

ODIN3 = os.environ.get("ODIN3_CLI")
VERILOG = """module m(a, y);
  wire _00_;
  wire [1:0] \\uut.x ;
  input a;
  output [1:0] y;
  NAND2 _05_ (
    .A(a),
    .Y(_00_)
  );
  assign y = _00_;
  OTHER _06_ (
endmodule
"""
BLIF = """# c
.model top
.inputs a[0] b
.outputs y
.subckt NAND2 A=a[0] B=b Y=n1
.names n1 y
1 1
.latch y q re clk 3
.end
.model other
.inputs zz
.end
"""


class NameTests(unittest.TestCase):
    def test_verilog_names(self) -> None:
        tmp = scratch(self, "ni-")
        (tmp / "m.v").write_text(VERILOG)
        with mock.patch.object(ni, "ROOT", tmp):
            wires, instances, types = ni.verilog_names(Path("m.v"))
        self.assertEqual(wires, {"_00_", "uut.x", "a", "y"})
        self.assertEqual(instances, {"_05_"})  # OTHER is not a cell of cmos.v
        self.assertEqual(types, {"NAND2"})

    def test_blif_nets_first_model_only(self) -> None:
        tmp = scratch(self, "ni-")
        (tmp / "m.blif").write_text(BLIF)
        with mock.patch.object(ni, "ROOT", tmp):
            nets = ni.blif_nets(Path("m.blif"))
        self.assertEqual(nets, {"a[0]", "b", "y", "n1", "q", "clk"})

    def test_unescape(self) -> None:
        self.assertEqual(ni.unescape("\\a.b"), "a.b")
        self.assertEqual(ni.unescape("a"), "a")

    def test_missing(self) -> None:
        rows = [("t", "wire", "a", ""), ("t", "net", "b", ""), ("u", "wire", "c", "")]
        self.assertEqual(ni.missing({"a", "b", "c"}, rows, "t", ("wire",)), ["b", "c"])


@unittest.skipUnless(ODIN3, "ODIN3_CLI not set (CTest python_tools)")
class PlantedTests(unittest.TestCase):
    def copy_fixtures(self) -> Path:
        tmp = scratch(self, "ni-")
        shutil.copytree(REPO_ROOT / "tests/netlist", tmp / "tests/netlist")
        return tmp

    def args(self, **extra: str) -> argparse.Namespace:
        equiv = str(REPO_ROOT / "tools/equiv-check/equiv-check")
        return argparse.Namespace(odin3=ODIN3, equiv_check=equiv, **extra)

    def test_equivalent_fixture_passes(self) -> None:
        tmp = self.copy_fixtures()
        with mock.patch.object(ni, "ROOT", tmp):
            self.assertEqual(ni.check_equiv(self.args(micro="cone", form="verilog")), 0)

    def test_planted_difference_fails(self) -> None:
        tmp = self.copy_fixtures()
        mapped = tmp / "tests/netlist/cone/mapped.v"
        text = mapped.read_text()
        self.assertIn("NAND2 ", text)
        mapped.write_text(text.replace("NAND2 ", "NOR2 ", 1))
        with mock.patch.object(ni, "ROOT", tmp):
            self.assertEqual(ni.check_equiv(self.args(micro="cone", form="verilog")), 1)

    def test_missing_name_fails(self) -> None:
        real = ni.dump_lines

        def without_a_wire(binary: str, read: str) -> tuple[list[tuple[str, str, str, str]], str]:
            rows, why = real(binary, read)
            return [row for row in rows if not (row[1] == "wire" and row[2] == "a")], why

        with mock.patch.object(ni, "dump_lines", without_a_wire):
            self.assertEqual(ni.check_names(self.args(micro="cone")), 1)


if __name__ == "__main__":
    unittest.main()
