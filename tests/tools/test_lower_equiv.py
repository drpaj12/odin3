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
from contextlib import AbstractContextManager
from pathlib import Path
from typing import Any
from unittest import mock

import lower_common as lc
import lower_diff as ld
import lower_equiv as le
import opt_equiv

from . import helpers

ODIN3 = os.environ.get("ODIN3_CLI", "")
YOSYS = os.environ.get("ODIN3_YOSYS055", "")
CAP = 3 << 30
HERE = Path(__file__).resolve().parent
TECHLIB = HERE.parent.parent / "lib" / "vtr.o3lib"
TWO_CLOCKS = """module m(input clk0, input clk1, input [1:0] a, output out);
  reg sel;
  reg [1:0] t;
  always @(posedge clk0) t <= a;
  always @(posedge clk1) sel <= sel + 1;
  assign out = sel ? t[1] : t[0];
endmodule
"""
HARD_BLOCK = """module m(input clk, input we, input [3:0] d, output [3:0] q);
  reg [3:0] a;
  always @(posedge clk) a <= a + 1;
  defparam i.ADDR_WIDTH = 4;
  defparam i.DATA_WIDTH = 4;
  single_port_ram i (.addr(a), .data(d), .we(we), .clk(clk), .out(q));
endmodule
"""

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
LOW = """module m(input [7:1] a, input [3:0] b, output [7:1] y, output [4:2] z);
  assign y = {{a[7:2] & {{b[3:0], b[1:0]}}, {y1}}};
  assign z = {zexpr};
endmodule
"""


def low(y1: str = "a[1] | b[0]", zexpr: str = "a[4:2] ^ b[2:0]") -> str:
    return LOW.format(y1=y1, zexpr=zexpr)


def ff(clk: str = "clk", rst: str = "rst", val: str = "4'b0101") -> str:
    return FF.format(clk=clk, body=RST_FIRST.format(rst=rst, val=val))


@unittest.skipUnless(ODIN3 and YOSYS and Path(YOSYS).exists() and Path(ODIN3).exists(),
                     "odin3 (ODIN3_CLI) and Yosys 0.55 (ODIN3_YOSYS055) are needed")
class RefinementTest(unittest.TestCase):
    """Correct lowerings refine (exit 0); each mutant of spec 14.4 does not."""

    def setUp(self) -> None:
        self.base = Path(tempfile.mkdtemp(prefix="test-lower-equiv-"))
        self.count = 0

    def snap(self, text: str, techlib: str | None = None) -> tuple[Path, Path]:
        """(lower-before.il, lower-after.blif) of a design given as Verilog text."""
        self.count += 1
        run = self.base / f"d{self.count}"
        run.mkdir()
        source = run / "m.v"
        source.write_text(text, encoding="utf-8")
        script = le.snapshot_script([str(source)], None, techlib, ["proc", "opt"], "", run)
        return le.snapshot(ODIN3, script, None, run, CAP)

    def verdict(self, gold: str, gate: str, techlib: str | None = None) -> le.Verdict:
        before, _ = self.snap(gold, techlib)
        _, after = self.snap(gate, techlib)
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

    def test_a_port_with_a_nonzero_low_index_compares_bit_for_bit(self) -> None:
        """H2 (3B-4 review): ``output [7:1]`` is a width-8 wide port in the BLIF; it must be
        matched to the gold's width-7 port at offset 1, not reported as a difference."""
        self.refines(low())
        self.refines(low(y1="1'b0", zexpr="a[4:2] & b[2:0]"))

    def test_a_wrong_bit_of_a_low_offset_port_differs(self) -> None:
        self.differs(low(), low(y1="a[1] & b[0]"))  # bit y[1], the port's lowest
        self.differs(low(), low(zexpr="a[4:2] ^ b[3:1]"))  # z[4:2], offset 2
        self.differs(low(), low().replace("a[7:2] &", "a[7:2] |"))  # y[7:2]

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

    def test_a_register_that_feeds_itself_is_paired_by_name_across_two_clocks(self) -> None:
        """The BLIF keeps no internal net name, so without the pairing hint the induction cannot
        relate the gate's `$n` register to the gold's `sel` (3B heavy triage, 2026-10-18:
        multiclock_output_and_latch was reported as not a refinement)."""
        self.refines(TWO_CLOCKS)

    def test_a_hard_block_fed_by_a_register_refines(self) -> None:
        """A `single_port_ram` has no model: its inputs are compared and its outputs shared
        (`expose -evert`), which needs the module declared on both sides (ch_intrinsics,
        mkPktMerge were reported as a port mismatch)."""
        got = self.verdict(HARD_BLOCK, HARD_BLOCK, str(TECHLIB))
        self.assertEqual(got.kind, "ok", got.detail)

    def test_a_hard_block_with_a_wrong_input_is_not_proved(self) -> None:
        wrong = HARD_BLOCK.replace("a <= a + 1", "a <= a + 2")
        got = self.verdict(HARD_BLOCK, wrong, str(TECHLIB))
        self.assertIn(got.kind, ("differs", "unproven"), got.detail)


GATE = """autoidx 1
module \\m
  wire width 3 input 1 \\a
  wire width 8 output 2 \\y
  wire width 2 offset 3 output 3 \\z
  wire \\y[1]
  cell $_BUF_ \\y.buf1
    connect \\A \\a [0]
    connect \\Y \\y[1]
  end
  connect \\y [7:1] { \\y[7] \\y[6] \\y[5] \\y[4] \\y[3] \\y[2] \\y[1] }
end
"""
GOLD = """autoidx 1
module \\m
  wire width 3 input 1 \\a
  wire width 7 offset 1 output 2 \\y
  wire width 2 offset 3 output 3 \\z
end
"""


class NormalisePortsTest(unittest.TestCase):
    """The port bridge of ``make_gate`` on RTLIL text (no tool runs)."""

    def test_a_wide_port_is_bridged_to_the_golds_shape(self) -> None:
        out = le.normalise_ports(GATE, GOLD, "\\m")
        self.assertIn("  wire width 8 \\y$wide\n", out)
        self.assertIn("  wire width 7 offset 1 output 2 \\y\n", out)
        self.assertIn("  connect \\y \\y$wide [7:1]\n", out)
        self.assertIn("  connect \\y$wide [7:1] { \\y[7]", out)  # the old uses are renamed
        self.assertIn("    connect \\Y \\y[1]\n", out)  # a bit wire named like it is not
        self.assertIn("  cell $_BUF_ \\y.buf1\n", out)  # nor a cell
        self.assertEqual(out.count("output 2"), 1)

    def test_a_port_that_already_matches_is_untouched(self) -> None:
        out = le.normalise_ports(GATE, GOLD, "\\m")
        self.assertIn("  wire width 2 offset 3 output 3 \\z\n", out)
        self.assertNotIn("z$wide", out)

    def test_a_port_with_a_missing_bit_is_left_to_the_port_match(self) -> None:
        gold = GOLD.replace("width 7 offset 1 output 2", "width 9 offset 1 output 2")
        self.assertEqual(le.normalise_ports(GATE, gold, "\\m"), GATE)


BOX = """attribute \\blackbox 1
attribute \\dynports 1
module \\ram
  parameter \\W 1
  wire input 1 \\a
  wire output 2 \\q
end
"""


class BlackboxTest(unittest.TestCase):
    """The black-box declarations copied into the gate (no tool runs)."""

    def test_the_modules_with_the_blackbox_attribute_are_found(self) -> None:
        text = "autoidx 1\nmodule \\m\n  cell \\ram \\i\n  end\nend\n" + BOX
        self.assertEqual(list(le.blackbox_modules(text)), ["\\ram"])
        self.assertEqual(le.blackbox_modules(text)["\\ram"], BOX)

    def test_a_used_declaration_the_gate_lacks_is_added_once(self) -> None:
        gate = "autoidx 1\nmodule \\m\n  cell \\ram \\i\n  end\nend\n"
        out = le.add_blackboxes(gate, gate + BOX)
        self.assertEqual(out, gate + BOX)
        self.assertEqual(le.add_blackboxes(out, gate + BOX), out)

    def test_an_unused_declaration_is_not_added(self) -> None:
        gate = "module \\m\nend\n"
        self.assertEqual(le.add_blackboxes(gate, gate + BOX), gate)


PAIR_GOLD = """autoidx 1
module \\m
  wire width 3 \\r
  wire \\s
  wire width 2 \\u
end
"""
PAIR_GATE = """autoidx 1
module \\m
  wire $n1
  cell $dff \\r.dff_p5
    connect \\Q $n5
  end
  cell $dff \\r.dff_p2
    connect \\Q $n2
  end
  cell $dff \\r.dff_p8
    connect \\Q $n8
  end
  cell $dff \\s.dff_p1
    connect \\Q $n1
  end
  cell $dff \\u.dff_p1
    connect \\Q $n9
  end
end
"""


class PairRegistersTest(unittest.TestCase):
    """The register pairing hint of ``make_gate`` on RTLIL text (no tool runs)."""

    def test_a_register_gets_its_golds_name_with_the_bits_in_order(self) -> None:
        out = le.pair_registers(PAIR_GATE, PAIR_GOLD)
        self.assertIn("  wire width 3 \\r\n  connect \\r [0] $n2\n  connect \\r [1] $n5\n"
                      "  connect \\r [2] $n8\n", out)
        self.assertIn("  wire \\s\n  connect \\s $n1\n", out)

    def test_the_bits_the_golds_cells_drive_take_the_cells_that_survived(self) -> None:
        """A register whose upper bits nothing reads keeps its width in the gold wire but fewer
        cells in the gate (multiclock_reader_writer's `register`)."""
        cell = "  cell $dff \\c\n    connect \\Q \\r [2:1]\n  end\n"
        gold = PAIR_GOLD.replace("end\n", cell + "end\n", 1)
        gate = PAIR_GATE.replace("  cell $dff \\r.dff_p8\n    connect \\Q $n8\n  end\n", "")
        out = le.pair_registers(gate, gold)
        self.assertIn("  connect \\r [1] $n2\n  connect \\r [2] $n5\n", out)
        self.assertNotIn("connect \\r [0]", out)

    def test_a_register_with_fewer_cells_than_the_golds_width_is_left_alone(self) -> None:
        self.assertNotIn("connect \\u ", le.pair_registers(PAIR_GATE, PAIR_GOLD))

    def test_a_dup_suffix_orders_cells_of_one_name(self) -> None:
        gate = PAIR_GATE.replace("r.dff_p5", "r.dff_p1_2").replace("r.dff_p2", "r.dff_p1_1")
        gate = gate.replace("r.dff_p8", "r.dff_p1")
        out = le.pair_registers(gate, PAIR_GOLD)
        self.assertIn("connect \\r [0] $n8\n  connect \\r [1] $n2\n  connect \\r [2] $n5\n", out)

    def test_unpaired_registers_are_the_cells_without_a_public_name(self) -> None:
        self.assertEqual(le.unpaired_registers(PAIR_GATE), 5)  # r (3 cells), s and u
        paired = le.pair_registers(PAIR_GATE, PAIR_GOLD)
        self.assertEqual(le.unpaired_registers(paired), 1)  # u: one cell for a width-2 wire

    def test_a_name_the_gate_already_has_is_not_declared_twice(self) -> None:
        gate = PAIR_GATE.replace("  wire $n1\n", "  wire $n1\n  wire \\s\n")
        self.assertEqual(le.pair_registers(gate, PAIR_GOLD).count("wire \\s\n"), 1)


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


def verdicts(*kinds: str) -> AbstractContextManager[Any]:
    """``check_design`` returning these verdict kinds in turn (no tool runs)."""
    return mock.patch.object(le, "check_design",
                             side_effect=[le.Verdict(kind, "d", 1) for kind in kinds])


class ExitCodeTest(unittest.TestCase):
    """``main``'s exit code from the verdicts (M3 of the 3B-4 review: exit 4 was never pinned)."""

    def run_main(self, *kinds: str) -> tuple[int, str]:
        base = Path(tempfile.mkdtemp(prefix="test-lower-equiv-exit-"))
        files = [str(base / f"d{n}.v") for n in range(len(kinds))]
        with verdicts(*kinds), mock.patch.object(ld, "check_yosys"):
            code, out, err = helpers.run_main(le.main, ["--odin3", "o", "--yosys", "y", "--work",
                                                        str(base / "w"), "--each", *files])
        return code, out + err

    def test_an_unproven_design_alone_is_exit_4(self) -> None:
        code, out = self.run_main("unproven")
        self.assertEqual(code, le.EXIT_UNPROVEN)
        self.assertEqual(le.EXIT_UNPROVEN, 4)
        self.assertIn("UNPROVEN", out)
        self.assertIn("lower-equiv: compared 0 of 1 designs (1 unproven, 0 empty)", out)

    def test_unproven_beside_a_proved_design_is_still_exit_4(self) -> None:
        self.assertEqual(self.run_main("ok", "unproven", "ok")[0], 4)

    def test_a_failure_beats_an_unproven_design(self) -> None:
        self.assertEqual(self.run_main("unproven", "differs")[0], 1)
        self.assertEqual(self.run_main("differs", "unproven")[0], 1)

    def test_proved_designs_are_exit_0_and_counted(self) -> None:
        code, out = self.run_main("ok", "ok")
        self.assertEqual(code, 0)
        self.assertIn("lower-equiv: compared 2 of 2 designs (0 unproven, 0 empty)", out)

    def test_an_empty_design_is_not_counted(self) -> None:
        code, out = self.run_main("empty")
        self.assertEqual(code, 1)
        self.assertIn("compared 0 of 1 designs (0 unproven, 1 empty)", out)


class RefineVerdictTest(unittest.TestCase):
    """``refine``'s decision table with the SAT flow and the cec stubbed (M1 of the review: only
    one real test reached the "no memory, the SAT flow failed" line)."""

    GOLD_PLAIN = "module \\m\n  cell $and \\c\n  end\nend\n"
    GOLD_MEM = "module \\m\n  cell $mem \\c\n  end\nend\n"
    GOLD_SEQ = "autoidx 1\nmodule \\m\n  cell $dff \\c\n  end\nend\n"
    # The gate: a flip-flop whose output has no public name, and one that has.
    GATE_UNPAIRED = ("autoidx 1\nmodule \\m\n  cell $dff \\r.dff_p1\n    connect \\Q $n1\n  end\n"
                     "end\n")
    GATE_PAIRED = GATE_UNPAIRED.replace("$n1", "\\r")

    def verdict(self, gold_text: str, sat_ok: bool, cec: str,
                gate_text: str = "autoidx 1\nmodule \\m\n  wire \\x\nend\n") -> tuple[str, str]:
        base = Path(tempfile.mkdtemp(prefix="test-lower-equiv-refine-"))
        gold = base / "gold.il"
        gold.write_text(gold_text, encoding="utf-8")
        (base / "gate.il").write_text(gate_text, encoding="utf-8")
        checker = mock.Mock()
        checker.refines.return_value = (sat_ok, "the SAT flow failed")
        with mock.patch.object(opt_equiv, "Checker", return_value=checker), \
                mock.patch.object(le, "cec_gold_and_blif", return_value=(cec, "cec text")):
            return le.refine("y", gold, base / "gate.il", base / "after.blif", base, 1, None)

    def test_a_sat_failure_without_a_memory_is_a_difference_whatever_the_cec_says(self) -> None:
        for cec in (opt_equiv.CEC_UNPROVEN, opt_equiv.CEC_DIFFERS):
            with self.subTest(cec=cec):
                self.assertEqual(self.verdict(self.GOLD_PLAIN, False, cec)[0], "differs")

    def test_a_sat_failure_with_a_memory_follows_the_cec(self) -> None:
        self.assertEqual(self.verdict(self.GOLD_MEM, False, opt_equiv.CEC_PROVED)[0], "ok")
        self.assertEqual(self.verdict(self.GOLD_MEM, False, opt_equiv.CEC_DIFFERS)[0], "differs")
        self.assertEqual(self.verdict(self.GOLD_MEM, False, opt_equiv.CEC_UNPROVEN)[0],
                         "unproven")

    def test_a_sat_failure_with_an_unpaired_register_follows_the_cec(self) -> None:
        """An induction that cannot pair the gate's registers proves no difference: `differs`
        only when the cec says so (3B heavy triage: two-clock designs were reported as not a
        refinement)."""
        got = [self.verdict(self.GOLD_SEQ, False, cec, self.GATE_UNPAIRED)[0]
               for cec in (opt_equiv.CEC_UNPROVEN, opt_equiv.CEC_DIFFERS, opt_equiv.CEC_PROVED)]
        self.assertEqual(got, ["unproven", "differs", "ok"])

    def test_a_sat_failure_with_every_register_paired_is_a_difference(self) -> None:
        self.assertEqual(self.verdict(self.GOLD_SEQ, False, opt_equiv.CEC_UNPROVEN,
                                      self.GATE_PAIRED)[0], "differs")

    def test_a_proof_never_asks_the_cec(self) -> None:
        self.assertEqual(self.verdict(self.GOLD_PLAIN, True, opt_equiv.CEC_DIFFERS)[0], "ok")

    def test_a_cec_proof_without_a_memory_proves_the_design(self) -> None:
        self.assertEqual(self.verdict(self.GOLD_PLAIN, False, opt_equiv.CEC_PROVED)[0], "ok")

    def test_a_sat_timeout_is_unproven_but_a_crash_is_not_absorbed(self) -> None:
        base = Path(tempfile.mkdtemp(prefix="test-lower-equiv-refine-"))
        checker = mock.Mock()
        checker.refines.side_effect = lc.CrashError("yosys", "timed out after 60 s", True)
        with mock.patch.object(opt_equiv, "Checker", return_value=checker):
            self.assertEqual(le.refine("y", base / "g", base / "h", base / "a", base, 1,
                                       None)[0], "unproven")
            checker.refines.side_effect = lc.CrashError("yosys", "killed by signal 11")
            with self.assertRaises(lc.CrashError):
                le.refine("y", base / "g", base / "h", base / "a", base, 1, None)


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

    def test_a_stale_snapshot_in_a_reused_work_directory_is_not_judged(self) -> None:
        """Review L1: odin3 wrote no snapshot, the work directory holds an old one: exit 2."""
        base = Path(tempfile.mkdtemp(prefix="test-lower-equiv-stale-"))
        yosys = base / "yosys"
        yosys.write_text("#!/usr/bin/env bash\necho 'Yosys 0.55 (fake)'\n", encoding="utf-8")
        yosys.chmod(yosys.stat().st_mode | stat.S_IXUSR)
        odin = base / "odin3"
        odin.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
        odin.chmod(odin.stat().st_mode | stat.S_IXUSR)
        for snap in ("snap", "snap-old"):  # the old name and any directory the tool might reuse
            stale = base / "w" / "d0000" / snap
            stale.mkdir(parents=True)
            (stale / le.BEFORE_NAME).write_text("module \\m\nend\n", encoding="utf-8")
            (stale / le.AFTER_NAME).write_text(".model m\n.end\n", encoding="utf-8")
        source = base / "m.v"
        source.write_text(COMB.format(zexpr="a | b"), encoding="utf-8")
        code, _, err = helpers.run_main(le.main, ["--odin3", str(odin), "--yosys", str(yosys),
                                                  "--work", str(base / "w"), str(source)])
        self.assertEqual(code, 2, err)
        self.assertIn("wrote no", err)

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
