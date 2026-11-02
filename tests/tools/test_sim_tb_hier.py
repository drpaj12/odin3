"""sim_tb: a DUT with several independent clock inputs, and read-only hierarchical references into
a DUT (`dut.u_leaf.r`). The designs and testbenches are written here.
Run through CTest `sim_tb_hier_flow` (ODIN3_BIN set); skipped without it.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ODIN3 = os.environ.get("ODIN3_BIN")

TWO_CLOCKS = """module two(input clka, input clkb, input [3:0] d, output reg [3:0] qa,
                output reg [3:0] qb);
  always @(posedge clka) qa <= d;
  always @(posedge clkb) qb <= d + 4'd1;
endmodule
"""

# Pulses one clock at a time: the other register must not move.
TWO_CLOCKS_TB = """module t;
  reg clka, clkb;
  reg [3:0] d;
  wire [3:0] qa, qb;
  two dut(.clka(clka), .clkb(clkb), .d(d), .qa(qa), .qb(qb));
  initial begin
    clka = 0; clkb = 0; d = 3;
    #1 clka = 1;
    #1 clka = 0;
    #1 $display("a only: %0d %0d", qa, qb);
    #1 clkb = 1;
    #1 clkb = 0;
    #1 $display("then b: %0d %0d", qa, qb);
    d = 9;
    #1 clka = 1;
    #1 clka = 0;
    #1 $display("a again: %0d %0d", qa, qb);
    $finish;
  end
endmodule
"""

# Two free-running clocks of different periods (the `always #N clk = ~clk` style).
COUNTERS = """module cnt(input clka, input clkb, output reg [7:0] ca, output reg [7:0] cb);
  initial begin ca = 0; cb = 0; end
  always @(posedge clka) ca <= ca + 8'd1;
  always @(posedge clkb) cb <= cb + 8'd1;
endmodule
"""

COUNTERS_TB = """`timescale 1ns/1ps
module t;
  reg clka = 0, clkb = 0;
  wire [7:0] ca, cb;
  cnt dut(.clka(clka), .clkb(clkb), .ca(ca), .cb(cb));
  always #5 clka = ~clka;
  always #7 clkb = ~clkb;
  initial begin
    #100 $display("%0d %0d", ca, cb);
    $finish;
  end
endmodule
"""

TWO_LEVEL = """module leaf(input clk, input [3:0] d, output [3:0] q);
  reg [3:0] r;
  always @(posedge clk) begin r <= d; end
  assign q = r;
endmodule
module top(input clk, input [3:0] d, output [3:0] q);
  leaf u_leaf(.clk(clk), .d(d), .q(q));
endmodule
"""

HIER_TB = """module t;
  reg clk = 0;
  reg [3:0] d = 0;
  wire [3:0] q;
  top dut(.clk(clk), .d(d), .q(q));
  initial begin
    d = 5;
    #1 clk = 1;
    #1 clk = 0;
    #1 $display("r=%0d q=%0d r0=%0d", dut.u_leaf.r, q, dut.u_leaf.r[0]);
    d = 6;
    #1 clk = 1;
    #1 clk = 0;
    #1 $display("r=%0d sum=%0d", dut.u_leaf.r, dut.u_leaf.r + 4'd1);
    $finish;
  end
endmodule
"""


def sim_tb(files: dict[str, str], tb_name: str, prefix: str = "proc; opt; lower; ",
           extra: str = "") -> subprocess.CompletedProcess[str]:
    assert ODIN3 is not None
    with tempfile.TemporaryDirectory() as tmp:
        paths = {}
        for name, text in files.items():
            paths[name] = Path(tmp) / name
            paths[name].write_text(text)
        dut = paths["dut.v"]
        out = Path(tmp) / "o.txt"
        script = (f"read_verilog {dut}; {extra}{prefix}sim_tb --out {out} {paths[tb_name]}")
        done = subprocess.run([ODIN3, "-p", script], capture_output=True, text=True)
        done.stdout = out.read_text() if out.exists() else ""
        return done


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest sim_tb_hier_flow)")
class MultiClockTest(unittest.TestCase):
    def test_each_clock_moves_only_its_own_registers(self) -> None:
        done = sim_tb({"dut.v": TWO_CLOCKS, "tb.v": TWO_CLOCKS_TB}, "tb.v")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertEqual(done.stdout.split("\n")[:3],
                         ["a only: 3 0", "then b: 3 4", "a again: 9 4"])

    def test_free_running_clocks_of_different_periods(self) -> None:
        # clka rises at 5, 15, ... 95 (10 edges); clkb at 7, 21, ... 91 (7 edges) before t = 100.
        done = sim_tb({"dut.v": COUNTERS, "tb.v": COUNTERS_TB}, "tb.v")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertEqual(done.stdout.split("\n")[0], "10 7")

    def test_more_clocks_than_the_cap_name_the_extra_one(self) -> None:
        n = 17
        ports = ", ".join(f"input c{k}" for k in range(n)) + ", input d, output o"
        dut = f"module m({ports});\n" + "".join(
            f"  reg r{k};\n  always @(posedge c{k}) r{k} <= d;\n" for k in range(n))
        dut += "  assign o = " + " ^ ".join(f"r{k}" for k in range(n)) + ";\nendmodule\n"
        conns = ", ".join(f".c{k}(k{k})" for k in range(n)) + ", .d(d), .o(o)"
        tb = ("module t;\n" + "".join(f"  reg k{k} = 0;\n" for k in range(n)) +
              f"  reg d = 1;\n  wire o;\n  m dut({conns});\n  initial #1 $finish;\nendmodule\n")
        done = sim_tb({"dut.v": dut, "tb.v": tb}, "tb.v")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("more than 16 clock inputs", done.stderr)
        self.assertIn("`c16` is one too many", done.stderr)


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest sim_tb_hier_flow)")
class HierarchicalReadTest(unittest.TestCase):
    def test_reads_a_signal_two_levels_down(self) -> None:
        done = sim_tb({"dut.v": TWO_LEVEL, "tb.v": HIER_TB}, "tb.v",
                      prefix="hierarchy --top top; proc; opt; lower; ")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertEqual(done.stdout.split("\n")[:2], ["r=5 q=5 r0=1", "r=6 sum=7"])

    def test_reads_after_flatten(self) -> None:
        done = sim_tb({"dut.v": TWO_LEVEL, "tb.v": HIER_TB}, "tb.v",
                      prefix="hierarchy --top top; proc; opt; flatten; opt; lower; ")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertEqual(done.stdout.split("\n")[:2], ["r=5 q=5 r0=1", "r=6 sum=7"])

    def test_unknown_signal_is_a_located_error(self) -> None:
        tb = HIER_TB.replace("dut.u_leaf.r,", "dut.u_leaf.nope,", 1)
        done = sim_tb({"dut.v": TWO_LEVEL, "tb.v": tb}, "tb.v",
                      prefix="hierarchy --top top; proc; opt; lower; ")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("has no signal `u_leaf.nope`", done.stderr)

    def test_write_to_a_hierarchical_name_is_refused(self) -> None:
        tb = HIER_TB.replace("d = 5;", "d = 5; dut.u_leaf.r = 1;", 1)
        done = sim_tb({"dut.v": TWO_LEVEL, "tb.v": tb}, "tb.v",
                      prefix="hierarchy --top top; proc; opt; lower; ")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("unsupported", done.stderr)

    def test_first_part_must_be_a_dut_instance(self) -> None:
        tb = HIER_TB.replace("dut.u_leaf.r,", "nobody.u_leaf.r,", 1)
        done = sim_tb({"dut.v": TWO_LEVEL, "tb.v": tb}, "tb.v",
                      prefix="hierarchy --top top; proc; opt; lower; ")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("first part is not a DUT instance", done.stderr)


if __name__ == "__main__":
    unittest.main()
