"""Tests for tools/hardmap-check/parmys_probe.py (3C Task 1, spec 9.2): the BLIF-to-Verilog wrapper
and the exit-code contract (0 match, 1 mismatch with the first cycle printed, 77 no iverilog)."""

from __future__ import annotations

import os
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import parmys_probe as pp

from .helpers import run_main

AND_SRC = "module t(input a, input b, output y);\n  assign y = a & b;\nendmodule\n"
AND_BLIF = ".model t\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n"
OR_BLIF = ".model t\n.inputs a b\n.outputs y\n.names a b y\n1- 1\n-1 1\n.end\n"

HAVE_IVERILOG = shutil.which("iverilog") is not None and shutil.which("vvp") is not None


class BlifToVerilogTests(unittest.TestCase):
    def test_blif_to_verilog_names(self) -> None:
        text = (
            ".model t\n.inputs x~0 x~1\n.outputs y\n.names z\n.names x~0 x~1 y\n11 1\n.end\n"
        )
        verilog = pp.blif_to_verilog(text)
        self.assertIn("input [1:0] x;", verilog)
        self.assertIn("output y;", verilog)
        self.assertRegex(verilog, r"assign n\d+ = 1'b0;")
        self.assertRegex(verilog, r"assign n\d+ = x\[1\];")

    def test_names_offset_cover(self) -> None:
        text = ".model t\n.inputs a\n.outputs y\n.names a y\n1 0\n.end\n"
        self.assertIn("~((n0))", pp.blif_to_verilog(text).replace(" ", ""))

    def test_blif_to_verilog_subckt(self) -> None:
        text = (
            ".model t\n.inputs a b c\n.outputs s k\n"
            ".subckt adder a=a b=b cin=c cout=k sumout=s\n.end\n"
        )
        verilog = pp.blif_to_verilog(text)
        self.assertRegex(verilog, r"adder\s+u0\(\.a\(n\d+\), \.b\(n\d+\), \.cin\(n\d+\), ")
        self.assertIn(".sumout(n", verilog)

    def test_ram_and_multiply_parameters(self) -> None:
        pins = ["addr[0]=a0", "addr[1]=a1", "data[0]=d", "out[0]=q", "clk=c", "we=w"]
        ram = ".subckt single_port_ram " + " ".join(pins)
        text = f".model t\n.inputs a0 a1 d c w\n.outputs q\n{ram}\n.end\n"
        self.assertIn("#(.ADDR_WIDTH(2), .DATA_WIDTH(1))", pp.blif_to_verilog(text))
        mul = ".subckt multiply a[0]=x a[1]=y b[0]=x out[0]=q"
        text = f".model t\n.inputs x y\n.outputs q\n{mul}\n.end\n"
        self.assertIn("#(.WIDTH(2))", pp.blif_to_verilog(text))

    def test_bad_port_name_is_an_error(self) -> None:
        with self.assertRaises(pp.ProbeError):
            pp.blif_to_verilog(".model t\n.inputs a.b\n.outputs y\n.end\n")


class ExitCodeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        (self.dir / "t.v").write_text(AND_SRC)

    def probe(self, blif: str, *extra: str) -> tuple[int, str, str]:
        (self.dir / "t.blif").write_text(blif)
        argv = [str(self.dir / "t.v"), "t", str(self.dir / "t.blif"), str(self.dir / "out"), *extra]
        return run_main(pp.main, ["--cycles", "200", *argv])

    @unittest.skipUnless(HAVE_IVERILOG, "iverilog not installed")
    def test_probe_exit_codes(self) -> None:
        code, out, _ = self.probe(AND_BLIF)
        self.assertEqual(code, 0, out)
        self.assertIn("200 cycles, 0 mismatches", out)
        code, out, _ = self.probe(OR_BLIF)
        self.assertEqual(code, 1, out)
        self.assertRegex(out, r"cycle \d+: y ref [01] blif [01]")

    @unittest.skipUnless(HAVE_IVERILOG, "iverilog not installed")
    def test_mask_holds_an_input(self) -> None:
        # With b masked to 0 the AND of a and b is 0, and so is the OR-less BLIF "a & b".
        code, out, _ = self.probe(AND_BLIF, "--mask", "b=0x0")
        self.assertEqual(code, 0, out)

    @unittest.skipUnless(HAVE_IVERILOG, "iverilog not installed")
    def test_hard_block_needs_primitives(self) -> None:
        blif = ".model t\n.inputs a b\n.outputs y\n.subckt adder a=a b=b cin=a cout=y\n.end\n"
        code, _, err = self.probe(blif, "--prim", str(self.dir / "missing.v"))
        self.assertEqual(code, 2)
        self.assertIn("primitives.v", err)

    def test_missing_iverilog_skips(self) -> None:
        with mock.patch.dict(os.environ, {"PATH": str(self.dir)}):
            code, _, err = self.probe(AND_BLIF)
        self.assertEqual(code, 77)
        self.assertIn("iverilog", err)

    def test_usage_error(self) -> None:
        code, _, _ = run_main(pp.main, ["only-one-argument"])
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
