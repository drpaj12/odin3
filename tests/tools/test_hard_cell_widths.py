"""A parameterised hard cell keeps its widths through write_rtlil, Yosys 0.55 and the bridge's
back end (2D-7b; single_port_ram / dual_port_ram of lib/vtr.o3lib).

``write_rtlil`` writes one stub per hard type with the ports at their *default* widths and
``attribute \\dynports 1`` (2E Task 3, review finding 3). Yosys's ``read_rtlil`` and ``hierarchy``
keep each instance's own widths for such a stub (the first test), but ``write_blif`` does not:
backends/blif/blif.cc:428 prints only ``min(connection width, the stub wire's width)`` bits of a
connection to a port the module has, so ``single_port_ram`` with ``DATA_WIDTH 8`` came out with
one-bit ``data`` and ``out``, and ``equiv-check`` rejected it ("port widths differ"). The bridge's
back end now deletes the black-box stubs before ``write_blif`` (the second test).

Needs ``ODIN3_CLI`` (CTest python_tools) and ``ODIN3_YOSYS055`` (the VTR build's Yosys 0.55);
skipped without them.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import elab_equiv

from . import helpers

ODIN3 = os.environ.get("ODIN3_CLI")
YOSYS = os.environ.get("ODIN3_YOSYS055")
TECHLIB = helpers.REPO_ROOT / "lib" / "vtr.o3lib"
EQUIV_CHECK = helpers.REPO_ROOT / "tools" / "equiv-check" / "equiv-check"
GIB = 1 << 30

SOURCE = """module t(input clk, input we, input [5:0] addr, input [7:0] d, output [7:0] q);
  single_port_ram #(.ADDR_WIDTH(6), .DATA_WIDTH(8)) m (.clk(clk), .we(we), .addr(addr),
                                                       .data(d), .out(q));
endmodule
"""


@unittest.skipUnless(ODIN3 and YOSYS, "ODIN3_CLI / ODIN3_YOSYS055 not set (CTest python_tools)")
class HardCellWidthsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.src = self.dir / "t.v"
        self.src.write_text(SOURCE, encoding="utf-8")

    def write_rtlil(self) -> Path:
        assert ODIN3
        il = self.dir / "a.il"
        script = f"read_techlib {TECHLIB}; read_verilog --nolatches {self.src}; write_rtlil {il}"
        done = subprocess.run([ODIN3, "-p", script], capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 0, done.stderr[-400:])
        return il

    def test_stub_is_dynports_with_default_widths_and_yosys_keeps_the_instance_widths(self) -> None:
        assert YOSYS
        il = self.write_rtlil()
        text = il.read_text(encoding="utf-8")
        self.assertIn("attribute \\dynports 1\n", text)
        self.assertRegex(text, r"module \\single_port_ram\n[\s\S]*wire input \d+ \\data\n")
        self.assertIn("    parameter \\DATA_WIDTH 8\n", text)
        script = f"read_rtlil {il}; hierarchy -check -top t; select -module t; write_rtlil"
        done = subprocess.run([YOSYS, "-q", "-p", script], capture_output=True, text=True,
                              check=False)
        self.assertEqual(done.returncode, 0, done.stderr[-400:])
        top = done.stdout[done.stdout.index("module \\t"):]
        self.assertRegex(top, r"wire width 8 input \d+ \\d\b")
        self.assertRegex(top, r"cell \\single_port_ram \\m[\s\S]*connect \\data \\d\n")
        self.assertRegex(top, r"connect \\out \\q\n")
        # the stub stays at the default widths (one bit): only the instance is 8 bits wide
        self.assertRegex(done.stdout, r"module \\single_port_ram\n[\s\S]*wire input \d+ \\data\n")

    def test_the_bridge_back_end_keeps_the_8_bit_ports_so_the_sides_are_equivalent(self) -> None:
        assert ODIN3 and YOSYS
        keep = self.dir / "keep"
        keep.mkdir()
        code, out, err = helpers.run_main(elab_equiv.main, [
            "--odin3", ODIN3, "--yosys", YOSYS, "--equiv-check", str(EQUIV_CHECK),
            "--techlib", str(TECHLIB), "--unmap-enables", "--mem-cap", str(3 * GIB),
            "--keep", str(keep), str(self.src)])
        self.assertEqual(code, 0, out + err)
        a_blif = (keep / "a.blif").read_text(encoding="utf-8")
        for bit in range(8):
            self.assertIn(f"data[{bit}]=d[{bit}]", a_blif)
            self.assertIn(f"out[{bit}]=q[{bit}]", a_blif)


if __name__ == "__main__":
    unittest.main()
