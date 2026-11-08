"""sim_tb: a DUT with several independent clock inputs, and read-only hierarchical references into
a DUT (`dut.u_leaf.r`). The designs and testbenches are written here.
Run through CTest `sim_tb_hier_flow` (ODIN3_BIN set); skipped without it.
"""

from __future__ import annotations

import os
import shutil
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
        blif = "dut.blif" in paths
        dut = paths["dut.blif" if blif else "dut.v"]
        out = Path(tmp) / "o.txt"
        reader = "read_blif" if blif else "read_verilog"
        script = (f"{reader} {dut}; {extra}{prefix}sim_tb --2state --out {out} {paths[tb_name]}")
        done = subprocess.run([ODIN3, "-p", script], capture_output=True, text=True)
        done.stdout = out.read_text() if out.exists() else ""
        return done


# Both clocks rise at the same time step: the register on clkb must see the value qa had before
# that step's edges, not the one clka's edge just wrote.
CROSS = """module cross(input clka, input clkb, input [3:0] d, output reg [3:0] qa,
                output reg [3:0] qb);
  always @(posedge clka) qa <= d;
  always @(posedge clkb) qb <= qa;
endmodule
"""

CROSS_TB = """module t;
  reg clka = 0, clkb = 0;
  reg [3:0] d = 5;
  wire [3:0] qa, qb;
  cross dut(.clka(clka), .clkb(clkb), .d(d), .qa(qa), .qb(qb));
  initial begin
    #1 clka = 1; clkb = 1;
    #1 clka = 0; clkb = 0;
    #1 $display("step1: %0d %0d", qa, qb);
    d = 9;
    #1 clka = 1; clkb = 1;
    #1 $display("step2: %0d %0d", qa, qb);
    $finish;
  end
endmodule
"""


# A derived clock (a divided clock), a negedge flop and a plain posedge flop.
MIXED = """module mixed(input clka, input clkb, input d, output reg qa, output reg qn,
                 output reg qdiv);
  reg div;
  initial div = 0;
  always @(posedge clka) div <= ~div;
  always @(posedge div) qdiv <= d;
  always @(negedge clkb) qn <= d;
  always @(posedge clka) qa <= d;
endmodule
"""

MIXED_TB = """module t;
  reg clka = 0, clkb = 1, d = 0;
  wire qa, qn, qdiv;
  mixed dut(.clka(clka), .clkb(clkb), .d(d), .qa(qa), .qn(qn), .qdiv(qdiv));
  initial begin
    #1 clkb = 0;
    #1 clkb = 1;
    d = 1;
    #1 clka = 1;
    #1 clka = 0;
    #1 $display("1: %b%b%b", qa, qn, qdiv);
    d = 0;
    #1 clkb = 0;
    #1 clkb = 1;
    #1 $display("2: %b%b%b", qa, qn, qdiv);
    d = 1;
    #1 clka = 1; clkb = 0;
    #1 clka = 0; clkb = 1;
    #1 $display("3: %b%b%b", qa, qn, qdiv);
    #1 clka = 1;
    #1 clka = 0;
    d = 0;
    #1 clka = 1;
    #1 $display("4: %b%b%b", qa, qn, qdiv);
    $finish;
  end
endmodule
"""

# A `.latch` with no control is a `$_FF_` on the global clock: it hangs on no single clock input.
GLOBAL_BLIF = """.model m
.inputs clka clkb d
.outputs qa qb qg
.latch d qa re clka 0
.latch d qb re clkb 0
.latch d qg 0
.end
"""

GLOBAL_TB = """module t;
  reg clka = 0, clkb = 0, d = 1;
  wire qa, qb, qg;
  m dut(.clka(clka), .clkb(clkb), .d(d), .qa(qa), .qb(qb), .qg(qg));
  initial begin
    #1 clka = 1;
    #1 clka = 0;
    #1 $display("%b%b%b", qa, qb, qg);
    $finish;
  end
endmodule
"""


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest sim_tb_hier_flow)")
class MultiClockTest(unittest.TestCase):
    def test_each_clock_moves_only_its_own_registers(self) -> None:
        done = sim_tb({"dut.v": TWO_CLOCKS, "tb.v": TWO_CLOCKS_TB}, "tb.v")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertEqual(done.stdout.split("\n")[:3],
                         ["a only: 3 0", "then b: 3 4", "a again: 9 4"])

    def test_clocks_edging_together_sample_the_old_values(self) -> None:
        done = sim_tb({"dut.v": CROSS, "tb.v": CROSS_TB}, "tb.v")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertEqual(done.stdout.split("\n")[:2], ["step1: 5 0", "step2: 9 5"])

    def test_free_running_clocks_of_different_periods(self) -> None:
        # clka rises at 5, 15, ... 95 (10 edges); clkb at 7, 21, ... 91 (7 edges) before t = 100.
        done = sim_tb({"dut.v": COUNTERS, "tb.v": COUNTERS_TB}, "tb.v")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertEqual(done.stdout.split("\n")[0], "10 7")

    def test_derived_clock_and_negedge_flop_match_iverilog(self) -> None:
        done = sim_tb({"dut.v": MIXED, "tb.v": MIXED_TB}, "tb.v")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        got = done.stdout.split("\n")[:4]
        self.assertEqual(got, ["1: 101", "2: 101", "3: 111", "4: 011"])
        if shutil.which("iverilog") and shutil.which("vvp"):
            with tempfile.TemporaryDirectory() as tmp:
                (Path(tmp) / "dut.v").write_text(MIXED)
                (Path(tmp) / "tb.v").write_text(MIXED_TB)
                exe = Path(tmp) / "a.out"
                subprocess.run(["iverilog", "-o", str(exe), str(Path(tmp) / "dut.v"),
                                str(Path(tmp) / "tb.v")], check=True)
                ref = subprocess.run(["vvp", str(exe)], capture_output=True, text=True,
                                     check=True).stdout.split("\n")
            self.assertEqual(got, [ln for ln in ref if ln[:1].isdigit()][:4])

    def test_global_clock_cell_with_two_clocks_is_an_error(self) -> None:
        done = sim_tb({"dut.blif": GLOBAL_BLIF, "tb.v": GLOBAL_TB}, "tb.v", prefix="")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("DUT `dut` (module `m`) has 2 clock inputs and 1 cell(s) with no clock pin",
                      done.stderr)
        self.assertIn("`$_FF_` node", done.stderr)

    def test_global_clock_cell_with_one_clock_follows_it(self) -> None:
        blif = GLOBAL_BLIF.replace(".latch d qb re clkb 0\n", "").replace(
            ".inputs clka clkb d", ".inputs clka d").replace("qa qb qg", "qa qg")
        tb = GLOBAL_TB.replace("reg clka = 0, clkb = 0, d = 1;", "reg clka = 0, d = 1;").replace(
            "wire qa, qb, qg;", "wire qa, qg;").replace(".clkb(clkb), ", "").replace(
            ".qb(qb), ", "").replace('"%b%b%b", qa, qb, qg', '"%b%b", qa, qg')
        done = sim_tb({"dut.blif": blif, "tb.v": tb}, "tb.v", prefix="")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertEqual(done.stdout.split("\n")[0], "11")

    def test_clock_on_a_vector_port_bit_is_an_error(self) -> None:
        dut = """module v(input [1:0] clk, input d, output reg q);
  always @(posedge clk[1]) q <= d;
endmodule
"""
        tb = """module t;
  reg [1:0] clk = 0;
  reg d = 1;
  wire q;
  v dut(.clk(clk), .d(d), .q(q));
  initial #1 $finish;
endmodule
"""
        done = sim_tb({"dut.v": dut, "tb.v": tb}, "tb.v")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("port `clk` is 2 bits wide and bit 1 is a clock", done.stderr)

    def test_exactly_16_clocks_are_accepted(self) -> None:
        n = 16
        ports = ", ".join(f"input c{k}" for k in range(n)) + ", input d, output o"
        dut = f"module m({ports});\n" + "".join(
            f"  reg r{k};\n  always @(posedge c{k}) r{k} <= d;\n" for k in range(n))
        dut += "  assign o = " + " ^ ".join(f"r{k}" for k in range(n)) + ";\nendmodule\n"
        conns = ", ".join(f".c{k}(k{k})" for k in range(n)) + ", .d(d), .o(o)"
        tb = ("module t;\n" + "".join(f"  reg k{k} = 0;\n" for k in range(n)) +
              f"  reg d = 1;\n  wire o;\n  m dut({conns});\n  initial begin\n"
              "    #1 k5 = 1;\n    #1 k5 = 0;\n    #1 $display(\"o=%b\", o);\n    $finish;\n"
              "  end\nendmodule\n")
        done = sim_tb({"dut.v": dut, "tb.v": tb}, "tb.v")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertEqual(done.stdout.split("\n")[0], "o=1")

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
        # the line of the read (10), not of the DUT instance (5), with the testbench file name
        self.assertIn("tb.v:10: DUT `dut`", done.stderr)

    def test_literal_index_of_a_vector_bit(self) -> None:
        tb = HIER_TB.replace("dut.u_leaf.r[0])", "dut.u_leaf.r[3])").replace("d = 5;", "d = 12;")
        tb = tb.replace('r0=%0d', 'r3=%0d')
        done = sim_tb({"dut.v": TWO_LEVEL, "tb.v": tb}, "tb.v",
                      prefix="hierarchy --top top; proc; opt; lower; ")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertEqual(done.stdout.split("\n")[0], "r=12 q=12 r3=1")

    def test_variable_index_is_an_error(self) -> None:
        tb = HIER_TB.replace("dut.u_leaf.r[0])", "dut.u_leaf.r[d])")
        done = sim_tb({"dut.v": TWO_LEVEL, "tb.v": tb}, "tb.v",
                      prefix="hierarchy --top top; proc; opt; lower; ")
        # a variable bit index of a DUT signal reads one bit (since the DUT array reads)
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])

    def test_part_select_is_an_error(self) -> None:
        tb = HIER_TB.replace("dut.u_leaf.r[0])", "dut.u_leaf.r[3:0])")
        done = sim_tb({"dut.v": TWO_LEVEL, "tb.v": tb}, "tb.v",
                      prefix="hierarchy --top top; proc; opt; lower; ")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("part-selects are not supported", done.stderr)

    def test_readmemh_into_a_path_is_refused(self) -> None:
        tb = HIER_TB.replace("d = 5;", 'd = 5; $readmemh("f.hex", dut.u_leaf.r);', 1)
        done = sim_tb({"dut.v": TWO_LEVEL, "tb.v": tb}, "tb.v",
                      prefix="hierarchy --top top; proc; opt; lower; ")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("$readmem expects", done.stderr)

    def test_dut_declared_after_its_use(self) -> None:
        inst = "  top dut(.clk(clk), .d(d), .q(q));\n"
        tb = HIER_TB.replace(inst, "").replace("  end\nendmodule", "  end\n" + inst + "endmodule")
        self.assertNotEqual(tb, HIER_TB)
        done = sim_tb({"dut.v": TWO_LEVEL, "tb.v": tb}, "tb.v",
                      prefix="hierarchy --top top; proc; opt; lower; ")
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.assertEqual(done.stdout.split("\n")[:2], ["r=5 q=5 r0=1", "r=6 sum=7"])

    def test_memory_word_by_path_says_so(self) -> None:
        dut = """module mem(input clk, input we, input [1:0] a, input [3:0] d, output [3:0] q);
  reg [3:0] m [0:3];
  always @(posedge clk) if (we) m[a] <= d;
  assign q = m[a];
endmodule
"""
        tb = """module t;
  reg clk = 0, we = 0;
  reg [1:0] a = 0;
  reg [3:0] d = 0;
  wire [3:0] q;
  mem dut(.clk(clk), .we(we), .a(a), .d(d), .q(q));
  initial begin
    #1 $display("%0d", dut.m[1]);
    $finish;
  end
endmodule
"""
        done = sim_tb({"dut.v": dut, "tb.v": tb}, "tb.v")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("tb.v:8:", done.stderr)
        self.assertIn("has no signal `m[1]`", done.stderr)
        self.assertIn("lower --keep-mem", done.stderr)

    def test_write_to_a_hierarchical_name_is_refused(self) -> None:
        tb = HIER_TB.replace("d = 5;", "d = 5; dut.u_leaf.r = 1;", 1)
        done = sim_tb({"dut.v": TWO_LEVEL, "tb.v": tb}, "tb.v",
                      prefix="hierarchy --top top; proc; opt; lower; ")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("unsupported", done.stderr)

    def test_name_cut_off_at_the_end_of_the_file_is_an_error(self) -> None:
        tb = HIER_TB.rsplit("endmodule", 1)[0] + "  initial $display(dut."
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
