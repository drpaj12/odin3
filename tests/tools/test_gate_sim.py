"""tools/silicon/gate_sim: reset-aware gate-level simulation (X start, 0 start, 1 start)."""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

import gate_sim as gs

PRIMS = """`timescale 1ns / 1ps
primitive sky130_fd_sc_hd__udp_dff$P (Q, D, CLK);
    output Q; input D; input CLK;
    reg Q;
    table
     //  D  CLK  :  Qt : Qt+1
         1  (01) :  ?  :  1 ;
         0  (01) :  ?  :  0 ;
         1  (x1) :  1  :  1 ;
         0  (x1) :  0  :  0 ;
         1  (0x) :  1  :  1 ;
         0  (0x) :  0  :  0 ;
         ?  (1x) :  ?  :  - ;
         ?  (?0) :  ?  :  - ;
         *   ?   :  ?  :  - ;
    endtable
endprimitive
"""
# A one-flop "PDK": q <= (q ^ q) | ~w. For w = 1 the next state is 0 whatever q was, but a gate
# simulator that starts q at X computes X ^ X = X and stays at X (X pessimism). The testbench
# changes w before the first clock edge: a UDP drives its initial state onto its output at the
# first input event.
CELLS = """module sky130_fd_sc_hd__dfxtp_1 (Q, CLK, D);
  output Q; input CLK; input D; wire b;
  sky130_fd_sc_hd__udp_dff$P d0 (b, D, CLK);
  buf (Q, b);
endmodule
"""
NETLIST = """module dut (input clk, input w, output q);
  wire a, nw, d;
  xor (a, q, q);
  not (nw, w);
  or (d, a, nw);
  sky130_fd_sc_hd__dfxtp_1 f (.Q(q), .CLK(clk), .D(d));
endmodule
"""
TB = """module tb;
  reg clk, w; wire q;
  dut #(.INIT(1)) u (.clk(clk), .w(w), .q(q));
  always #5 clk = ~clk;
  initial begin
    clk = 0; w = 0;
    #1 w = 1;
    repeat (3) @(posedge clk);
    #1 if (q === 1'b0) $display("PASS tb"); else $display("FAIL q=%b", q);
    $finish;
  end
endmodule
"""


class PureTest(unittest.TestCase):
    def test_patch_primitives(self) -> None:
        text = gs.patch_primitives(PRIMS, "1")
        self.assertIn("reg Q;\n    initial Q = 1'b1;", text)
        self.assertEqual(text.count("initial Q"), 1)
        with self.assertRaises(ValueError):
            gs.patch_primitives(PRIMS, "x")

    def test_strip_param(self) -> None:
        tb = "i2c_top #(.ARST_LVL (0)) u (.a(b)); i2c_top #(.OTHER(1)) v (.a(b));"
        out = gs.strip_param(tb, "ARST_LVL")
        self.assertEqual(out, "i2c_top  u (.a(b)); i2c_top #(.OTHER(1)) v (.a(b));")

    def test_verdict(self) -> None:
        self.assertEqual(gs.verdict({"x": True, "0": True, "1": True}), "pass")
        self.assertEqual(gs.verdict({"x": False, "0": True, "1": True}), "x-pessimism")
        self.assertEqual(gs.verdict({"x": False, "0": True, "1": False}), "fail")
        self.assertEqual(gs.verdict({"x": False, "0": False, "1": False}), "fail")


@unittest.skipUnless(shutil.which("iverilog") and shutil.which("vvp"), "needs iverilog")
class SimulateTest(unittest.TestCase):
    def test_x_pessimism_is_told_from_a_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            pdk = d / "pdk"
            pdk.mkdir()
            (pdk / "primitives.v").write_text(PRIMS)
            (pdk / "sky130_fd_sc_hd.v").write_text(CELLS)
            (d / "final.v").write_text(NETLIST)
            (d / "tb.v").write_text(TB.replace("#(.INIT(1)) ", ""))
            res = gs.simulate(d / "final.v", pdk, [d / "tb.v"], "tb", d / "out", "PASS tb")
            self.assertEqual(res["verdict"], "x-pessimism", res)

    def test_param_override_is_dropped(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            pdk = d / "pdk"
            pdk.mkdir()
            (pdk / "primitives.v").write_text(PRIMS)
            (pdk / "sky130_fd_sc_hd.v").write_text(CELLS)
            (d / "final.v").write_text(NETLIST)
            (d / "tb.v").write_text(TB)
            res = gs.simulate(d / "final.v", pdk, [d / "tb.v"], "tb", d / "out", "PASS tb",
                              params=["INIT=1"])
            self.assertEqual(res["verdict"], "x-pessimism", res)


if __name__ == "__main__":
    unittest.main()
