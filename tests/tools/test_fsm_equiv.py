"""Tests for tools/fsm-check/fsm_equiv.py (4.8 plan Task 2, spec section 15).

The Yosys 0.55 checks need the VTR yosys binary and skip without it.  The hand-written pair is a
3-state FSM and a one-hot copy of it; the wrong copy ends in the wrong state.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import fsm_equiv
import fsm_probe

from . import helpers

RTL = """\
module t(input clk, input rst, input go, output o);
  reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
      default: st <= 2'd0;
    endcase
  assign o = (st == 2'd2);
endmodule
"""

ONE_HOT = """\
module t(input clk, input rst, input go, output o);
  reg [2:0] oh;
  always @(posedge clk or posedge rst)
    if (rst) oh <= 3'b001;
    else oh <= {oh[1], oh[0] & go, (oh[0] & ~go) | oh[2]};
  assign o = oh[2];
endmodule
"""

WRONG = ONE_HOT.replace("assign o = oh[2];", "assign o = oh[1];")

YOSYS = fsm_probe.default_yosys()
HAVE_YOSYS = Path(YOSYS).is_file()


class ScriptTest(unittest.TestCase):
    def test_miter_script_shape(self) -> None:
        s = fsm_equiv.miter_script("g.v", "g.v", "t", ("rst", 1), 12)
        self.assertIn("miter -equiv -flatten -make_assert gold gate m; async2sync", s)
        self.assertIn(
            "sat -verify -seq 12 -set-init-zero -set-at 1 in_rst 1 -set-at 2 in_rst 1 "
            "-prove-asserts m",
            s,
        )
        self.assertIn("proc; fsm; opt", s)

    def test_active_low_reset_and_no_reset(self) -> None:
        low = fsm_equiv.miter_script("g.v", "g.v", "t", ("rstn", 0), 4)
        self.assertIn("-set-at 1 in_rstn 0 -set-at 2 in_rstn 0", low)
        none = fsm_equiv.miter_script("g.v", "g.v", "t", None, 4)
        self.assertNotIn("-set-at", none)

    def test_parse_rst(self) -> None:
        self.assertEqual(fsm_equiv.parse_rst("rst"), ("rst", 1))
        self.assertEqual(fsm_equiv.parse_rst("rstn:0"), ("rstn", 0))
        self.assertIsNone(fsm_equiv.parse_rst("-"))
        with self.assertRaises(fsm_equiv.EquivError):
            fsm_equiv.parse_rst("rst:2")

    def test_rtlil_read(self) -> None:
        self.assertEqual(fsm_equiv.read_cmd("a.il"), "read_rtlil a.il")
        with self.assertRaises(fsm_equiv.EquivError):
            fsm_equiv.read_cmd("a.txt")

    def test_micro_rows_are_complete(self) -> None:
        rows = fsm_equiv.read_rows(helpers.REPO_ROOT / "tests/fsm/micros.txt")
        self.assertGreaterEqual(len(rows), 30)
        for row in rows:
            self.assertTrue((helpers.REPO_ROOT / row.path).is_file(), row.path)
        names = {r.name for r in rows}
        for stem in ("states31", "states32", "states33", "enc_none", "keep_reg", "self_reset"):
            self.assertIn(stem, names)

    def test_bad_row_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "micros.txt"
            bad.write_text("a b c\n")
            with self.assertRaises(fsm_equiv.EquivError):
                fsm_equiv.read_rows(bad)

    def test_missing_yosys_is_exit_2(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "t.v"
            src.write_text(RTL)
            code, _, err = helpers.run_main(
                fsm_equiv.main, [str(src), "--top", "t", "--yosys", str(Path(tmp) / "nope")]
            )
        self.assertEqual(code, fsm_equiv.EXIT_ERROR)
        self.assertIn("yosys", err)


class OdinLegTest(unittest.TestCase):
    def test_odin_script_writes_gold_then_gate(self) -> None:
        script = fsm_equiv.odin_script("a.v", Path("g.il"), Path("t.il"))
        self.assertTrue(script.startswith("read_verilog --nolatches a.v; proc; write_rtlil g.il;"))
        self.assertTrue(script.endswith("fsm; opt; write_rtlil t.il"))

    def test_missing_odin_is_exit_2(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "t.v"
            src.write_text(RTL)
            with self.assertRaises(FileNotFoundError):
                fsm_equiv.odin_check(str(Path(tmp) / "nope"), YOSYS, str(src), ("t", "rst", "4"))


@unittest.skipUnless(HAVE_YOSYS, "needs the VTR yosys (Yosys 0.55)")
class YosysTest(unittest.TestCase):
    def run_pair(self, gate: str, rst: str | None) -> int:
        with tempfile.TemporaryDirectory() as tmp:
            gold_path, gate_path = Path(tmp) / "gold.v", Path(tmp) / "gate.v"
            gold_path.write_text(RTL)
            gate_path.write_text(gate)
            argv = [
                str(gold_path),
                "--gate",
                str(gate_path),
                "--top",
                "t",
                "--depth",
                "10",
                "--gold-script",
                "proc",
                "--gate-script",
                "proc",
                "--yosys",
                YOSYS,
            ]
            if rst is not None:
                argv += ["--rst", rst]
            return helpers.run_main(fsm_equiv.main, argv)[0]

    def test_hand_recoded_one_hot_is_equivalent_after_reset(self) -> None:
        self.assertEqual(self.run_pair(ONE_HOT, "rst:1"), fsm_equiv.EXIT_EQUIVALENT)

    def test_wrong_copy_is_not_equivalent(self) -> None:
        self.assertEqual(self.run_pair(WRONG, "rst:1"), fsm_equiv.EXIT_DIFFERENT)

    def test_reset_constraint_is_load_bearing(self) -> None:
        # Without the constraint the all-zero power-up of the one-hot copy is an illegal state.
        self.assertEqual(self.run_pair(ONE_HOT, None), fsm_equiv.EXIT_DIFFERENT)

    def test_yosys_fsm_on_the_rtl_is_equivalent_from_reset(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "t.v"
            src.write_text(RTL.replace("      default: st <= 2'd0;\n", ""))
            code = helpers.run_main(
                fsm_equiv.main, [str(src), "--top", "t", "--rst", "rst", "--yosys", YOSYS]
            )[0]
        self.assertEqual(code, fsm_equiv.EXIT_EQUIVALENT)


if __name__ == "__main__":
    unittest.main()
