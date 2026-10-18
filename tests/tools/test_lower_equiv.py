"""Tests for tools/lower-check/lower-equiv (3B Task 9, spec 14.4): ``lower`` refines its input.

The real ``odin3`` (``ODIN3_CLI``) and Yosys 0.55 (``ODIN3_YOSYS055``) run every case; the class is
skipped without them. A mutant is a second Verilog design with the same ports: its lowered BLIF is
checked against the first design's ``lower-before.il``, so the gate differs from the gold exactly
as a wrong lowering would (spec 14.4's regression mutants). The work directories are made by
``tempfile.mkdtemp`` and left in place (these tools never delete).
"""

from __future__ import annotations

import os
import stat
import tempfile
import unittest
from pathlib import Path

import lower_common as lc
import lower_equiv as le

from . import helpers

ODIN3 = os.environ.get("ODIN3_CLI", "")
YOSYS = os.environ.get("ODIN3_YOSYS055", "")
CAP = 3 << 30
HERE = Path(__file__).resolve().parent

FF = """module m(input clk, input clk2, input rst, input en, input [3:0] d, output reg [3:0] q);
  always @(posedge {clk}) {body}
endmodule
"""
RST_FIRST = "if ({rst}) q <= {val}; else if (en) q <= d;"
EN_FIRST = "if (en) q <= d; else if (rst) q <= 4'b0101;"
PMUX = """module m(input [2:0] s, input [3:0] a, input [3:0] b, input [3:0] c, output reg [3:0] y);
  always @* begin
    y = 4'b0;
    (* parallel_case *) case (1'b1)
      s[0]: y = a;
      s[1]: y = {one};
      s[2]: y = {two};
    endcase
  end
endmodule
"""
CMP = """module m(input {sign} [3:0] a, input {sign} [3:0] b, output y);
  assign y = a < b;
endmodule
"""
SHR = """module m(input [5:0] a, input [2:0] b, output [5:0] y);
  assign y = {expr};
endmodule
"""
MEM = """module m(input clk, input we, input [2:0] wa, input [3:0] wd, input [2:0] ra,
  output [3:0] rd);
  reg [3:0] mem [{lo}:{hi}];
  always @(posedge clk) if (we) mem[wa] <= wd;
  assign rd = mem[ra];
endmodule
"""
COMB = """module m(input [3:0] a, input [3:0] b, output [3:0] y, output [3:0] z);
  assign y = a & b;
  assign z = {zexpr};
endmodule
"""


def ff(clk: str = "clk", rst: str = "rst", val: str = "4'b0101") -> str:
    return FF.format(clk=clk, body=RST_FIRST.format(rst=rst, val=val))


@unittest.skipUnless(ODIN3 and YOSYS and Path(YOSYS).exists() and Path(ODIN3).exists(),
                     "odin3 (ODIN3_CLI) and Yosys 0.55 (ODIN3_YOSYS055) are needed")
class RefinementTest(unittest.TestCase):
    """Correct lowerings refine (exit 0); each mutant of spec 14.4 does not."""

    def setUp(self) -> None:
        self.base = Path(tempfile.mkdtemp(prefix="test-lower-equiv-"))
        self.count = 0

    def snap(self, text: str) -> tuple[Path, Path]:
        """(lower-before.il, lower-after.blif) of a design given as Verilog text."""
        self.count += 1
        run = self.base / f"d{self.count}"
        run.mkdir()
        source = run / "m.v"
        source.write_text(text, encoding="utf-8")
        script = le.snapshot_script([str(source)], None, None, ["proc", "opt"], "", run)
        return le.snapshot(ODIN3, script, None, run, CAP)

    def verdict(self, gold: str, gate: str) -> le.Verdict:
        before, _ = self.snap(gold)
        _, after = self.snap(gate)
        work = self.base / f"check{self.count}"
        work.mkdir()
        return le.check_pair(YOSYS, before, after, work, CAP, (None, None, False))

    def refines(self, text: str) -> None:
        got = self.verdict(text, text)
        self.assertEqual(got.kind, "ok", got.detail)
        self.assertGreater(got.cells, 0)

    def differs(self, gold: str, gate: str) -> None:
        got = self.verdict(gold, gate)
        self.assertEqual(got.kind, "differs", f"{got.kind}: {got.detail}")

    def test_correct_lowerings_refine(self) -> None:
        for text in (ff(), FF.format(clk="clk", body=EN_FIRST),
                     PMUX.format(one="b", two="c"), CMP.format(sign="signed"),
                     CMP.format(sign=""), SHR.format(expr="a >> b"),
                     MEM.format(lo=0, hi=7), COMB.format(zexpr="a | b")):
            with self.subTest(text=text.splitlines()[0] + text.splitlines()[1]):
                self.refines(text)

    def test_a_flipped_edge(self) -> None:
        self.differs(ff(), ff().replace("posedge", "negedge"))

    def test_a_wrong_clock_net(self) -> None:
        self.differs(ff(), ff(clk="clk2"))

    def test_a_wrong_function(self) -> None:
        self.differs(COMB.format(zexpr="a | b"), COMB.format(zexpr="a ^ b"))

    def test_a_wrong_reset_value(self) -> None:
        self.differs(ff(), ff(val="4'b0111"))

    def test_a_wrong_reset_polarity(self) -> None:
        self.differs(ff(), ff(rst="!rst"))

    def test_a_tied_output(self) -> None:
        self.differs(COMB.format(zexpr="a | b"), COMB.format(zexpr="a | b").replace(
            "assign y = a & b;", "assign y = {a[3:1] & b[3:1], 1'b0};"))

    def test_a_missing_port(self) -> None:
        gate = COMB.format(zexpr="a | b").replace("output [3:0] z", "output [2:0] z").replace(
            "assign z = a | b;", "assign z = a[2:0] | b[2:0];")
        self.differs(COMB.format(zexpr="a | b"), gate)

    def test_reset_and_enable_swapped(self) -> None:
        self.differs(ff(), FF.format(clk="clk", body=EN_FIRST))

    def test_a_bit_swapped_pmux(self) -> None:
        self.differs(PMUX.format(one="b", two="c"), PMUX.format(one="c", two="b"))

    def test_a_signed_comparison_read_unsigned(self) -> None:
        self.differs(CMP.format(sign="signed"), CMP.format(sign=""))

    def test_a_shr_shifted_by_one(self) -> None:
        self.differs(SHR.format(expr="a >> b"), SHR.format(expr="(a >> b) >> 1"))

    def test_a_wrong_mem_offset(self) -> None:
        self.differs(MEM.format(lo=1, hi=4), MEM.format(lo=2, hi=5))


class ReportTest(unittest.TestCase):
    """The expect list and the verdict lines (no tool runs)."""

    def test_a_listed_difference_is_accepted_and_a_stale_entry_fails(self) -> None:
        table = le.parse_expect("# c\nmem_x.v refine Yosys wraps the address\n")
        line, code = le.report_line("mem_x.v", "mem_x.v", le.Verdict("differs", "x"), table)
        self.assertEqual(code, 0)
        self.assertIn("as listed", line)
        line, code = le.report_line("mem_x.v", "mem_x.v", le.Verdict("ok", "", 1), table)
        self.assertEqual(code, 1)
        self.assertIn("stale", line)

    def test_unlisted_difference_unproven_and_empty(self) -> None:
        self.assertEqual(le.report_line("a.v", "a.v", le.Verdict("differs", "d"), {})[1], 1)
        self.assertEqual(le.report_line("a.v", "a.v", le.Verdict("unproven", "t"), {})[1],
                         le.EXIT_UNPROVEN)
        self.assertEqual(le.report_line("a.v", "a.v", le.Verdict("empty"), {})[1], 1)
        self.assertEqual(le.report_line("a.v", "a.v", le.Verdict("empty"), {}, True)[1], 0)

    def test_a_malformed_expect_line_is_a_tool_error(self) -> None:
        with self.assertRaises(lc.ToolError):
            le.parse_expect("only_two_fields refine\n")


@unittest.skipUnless(ODIN3 and Path(ODIN3).exists(), "odin3 (ODIN3_CLI) is needed")
class HardFailureTest(unittest.TestCase):
    """A crashing Yosys child is exit 3 and no expect list absorbs it."""

    def test_a_crashing_yosys_is_exit_3(self) -> None:
        base = Path(tempfile.mkdtemp(prefix="test-lower-equiv-hard-"))
        fake = base / "yosys"
        fake.write_text("#!/usr/bin/env bash\n"
                        "if [ \"$1\" = -V ]; then echo 'Yosys 0.55 (fake)'; exit 0; fi\n"
                        "kill -SEGV $$\n", encoding="utf-8")
        fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
        source = base / "m.v"
        source.write_text(COMB.format(zexpr="a | b"), encoding="utf-8")
        expect = base / "expect.txt"
        expect.write_text("m.v refine pretend this is expected\n", encoding="utf-8")
        code, _, err = helpers.run_main(le.main, ["--odin3", ODIN3, "--yosys", str(fake),
                                                  "--work", str(base / "w"), "--expect",
                                                  str(expect), str(source)])
        self.assertEqual(code, 3, err)
        self.assertIn("HARD", err)

    def test_a_failing_odin3_is_exit_2(self) -> None:
        base = Path(tempfile.mkdtemp(prefix="test-lower-equiv-err-"))
        fake = base / "yosys"
        fake.write_text("#!/usr/bin/env bash\necho 'Yosys 0.55 (fake)'\n", encoding="utf-8")
        fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
        code, _, err = helpers.run_main(le.main, ["--odin3", ODIN3, "--yosys", str(fake),
                                                  "--work", str(base / "w"),
                                                  str(base / "missing.v")])
        self.assertEqual(code, 2, err)


if __name__ == "__main__":
    unittest.main()
