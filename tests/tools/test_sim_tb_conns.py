"""sim_tb accepts exactly 4096 named port connections on a DUT instance (the cap; one more is the
located error of tests/tb/errors/manyconns_tb.v). The design and testbench are generated here.
Run through CTest `sim_tb_conns_flow` (ODIN3_BIN set); skipped without it.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ODIN3 = os.environ.get("ODIN3_BIN")
HALF = 2048  # 2048 inputs and 2048 outputs: 4096 connections


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest sim_tb_conns_flow)")
class ManyConnectionsTest(unittest.TestCase):
    def test_exactly_4096_connections_simulate(self) -> None:
        assert ODIN3 is not None
        ports = [f"input i{k}" for k in range(HALF)] + [f"output o{k}" for k in range(HALF)]
        dut = "module m(" + ", ".join(ports) + ");\n"
        dut += "".join(f"  assign o{k} = ~i{k};\n" for k in range(HALF)) + "endmodule\n"
        conns = [f".i{k}(a{k})" for k in range(HALF)] + [f".o{k}(y{k})" for k in range(HALF)]
        decls = "".join(f"  reg a{k};\n  wire y{k};\n" for k in range(HALF))
        tb = ("module t;\n" + decls + "  m dut(" + ", ".join(conns) + ");\n"
              "  initial begin\n    a0 = 0;\n    a2047 = 0;\n    #1;\n"
              "    $display(\"%b %b\", y0, y2047);\n"
              "    a0 = 1;\n    a2047 = 1;\n    #1;\n"
              "    $display(\"%b %b\", y0, y2047);\n"
              "    $finish;\n  end\nendmodule\n")
        with tempfile.TemporaryDirectory() as tmp:
            dut_file, tb_file, out = Path(tmp) / "m.v", Path(tmp) / "t_tb.v", Path(tmp) / "o.txt"
            dut_file.write_text(dut)
            tb_file.write_text(tb)
            done = subprocess.run(
                [ODIN3, "-p", f"read_verilog {dut_file}; proc; opt; lower; "
                 f"sim_tb --out {out} {tb_file}"], capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr[-2000:])
            self.assertEqual(out.read_text().split("\n")[:2], ["1 1", "0 0"])


if __name__ == "__main__":
    unittest.main()
