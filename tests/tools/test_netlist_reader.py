"""V1.2.0 follow-up: `read_verilog -netlist` keeps what write_verilog wrote (one test per reason the
Verilog round trip failed on a micro), and the same text read without -netlist (the RTL path) is
unchanged. Through the odin3 CLI (ODIN3_CLI).

  - proc (step F, opt_expr) leaves the cells of a module read with -netlist alone: `a ^ 1'b0` stays;
    read as source it folds;
  - a constant operator (`~1'b0`) is a cell under -netlist and folds to a constant otherwise;
  - the cell of `assign z = 1'b1;  // t.v:9` has that location under -netlist (the module's line
    otherwise);
  - `~(a ^ b)` is one $_XNOR_ gate under -netlist (written back as it was) and a $xor and a $not
    otherwise.
"""

from __future__ import annotations

import unittest
from pathlib import Path

from .restore_common import LIB, ODIN3, run_cli, workdir

SOURCE = """module t (input clk, input a, input b, input c, output q, output z);
  wire x;
  wire y;
  wire w;
  wire k;
  assign x = a ^ 1'b0;  // t.v:5
  assign y = c ? a : 1'b0;  // t.v:6
  assign w = ~1'b0;  // t.v:7
  assign k = ~(a ^ b);  // t.v:8
  assign z = 1'b1;  // t.v:9
  reg r;
  always @(posedge clk) r <= x | y | w | k;
  assign q = r;
endmodule
"""
KEPT = (
    "assign x = a ^ 1'b0;  // t.v:5",
    "assign y = c ? a : 1'b0;  // t.v:6",
    "assign w = ~1'b0;  // t.v:7",
    "assign k = ~(a ^ b);  // t.v:8",
    "assign z = 1'b1;  // t.v:9",
)


def write_back(src: Path, out: Path, option: str) -> str:
    script = f"read_techlib {LIB}; read_verilog {option} {src}; proc; write_verilog {out}"
    done = run_cli(script)
    assert done.returncode == 0, done.stderr
    return out.read_text()


@unittest.skipIf(ODIN3 is None, "skipped 'ODIN3_CLI is not set'")
class NetlistReader(unittest.TestCase):
    def setUp(self) -> None:
        self.work = workdir(self, "netlist-reader")
        self.src = self.work / "t.v"
        self.src.write_text(SOURCE)

    def test_netlist_keeps_every_line_and_a_second_pass_is_the_same_file(self) -> None:
        first = write_back(self.src, self.work / "1.v", "-netlist")
        for line in KEPT:
            self.assertIn("  " + line, first.splitlines())
        second = write_back(self.work / "1.v", self.work / "2.v", "-netlist")
        self.assertEqual(first, second)

    def test_without_netlist_proc_folds_and_the_gate_is_two_cells(self) -> None:
        out = write_back(self.src, self.work / "src.v", "")
        for line in (KEPT[0], KEPT[2], KEPT[3]):  # the mux with a constant is kept by opt_expr
            self.assertNotIn(line.split("  //")[0], out, line)
        self.assertIn("assign x = a;", out)  # step F: x ^ 1'b0 is x
        self.assertIn("= a ^ b;", out)  # the $xor ...
        self.assertRegex(out, r"assign k = ~\\\$n\d+ ;")  # ... and its $not

    def test_the_constant_cell_has_the_location_of_its_assign_under_netlist_only(self) -> None:
        netlist = write_back(self.src, self.work / "n.v", "-netlist")
        self.assertIn("assign z = 1'b1;  // t.v:9", netlist)
        plain = write_back(self.src, self.work / "p.v", "")
        const = [x for x in plain.splitlines() if x.startswith("  assign w = 1'b1;")]
        self.assertEqual(1, len(const))
        self.assertTrue(const[0].endswith("t.v:1"), const[0])

    def test_negated_reduction_of_a_constant_keeps_its_negation_under_netlist(self) -> None:
        src = self.work / "r.v"
        src.write_text(
            "module t (output z, output y);\n  assign z = ~&2'b11;\n"
            "  assign y = ~|2'b00;\nendmodule\n"
        )
        out = write_back(src, self.work / "r1.v", "-netlist")
        self.assertNotIn("assign z = &2'b11", out)
        self.assertNotIn("assign y = |2'b00", out)
        self.assertEqual(out, write_back(self.work / "r1.v", self.work / "r2.v", "-netlist"))
        plain = write_back(src, self.work / "rp.v", "")
        self.assertIn("assign z = 1'b0;", plain)
        self.assertIn("assign y = 1'b1;", plain)

    def test_multi_bit_xnor_is_not_a_gate_under_netlist(self) -> None:
        src = self.work / "x.v"
        src.write_text(
            "module t (input [1:0] a, input [1:0] b, output [1:0] k);\n"
            "  assign k = ~(a ^ b);\nendmodule\n"
        )
        out = write_back(src, self.work / "x1.v", "-netlist")
        self.assertNotIn("_XNOR_", out)


if __name__ == "__main__":
    unittest.main()
