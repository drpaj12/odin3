"""Tests for tools/icells-check/icells_check.py (5.2 spec sections 7 to 9): the instance parser and
comparison (a planted parameter change is found), the expect file (stale and wrong entries), the
script text of a level, the census, the exit codes and the per-design report, with the Odin III
runs replaced by fixed verdicts.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import icells_check as ic

from .helpers import run_main

WRITTEN = """module m (input [3:0] a, output [3:0] y);
  wire \\$n5 ;
  \\$pow #(.A_SIGNED(0), .B_SIGNED(0), .A_WIDTH(4), .B_WIDTH(4), .Y_WIDTH(4)) \\$c21 (  // m.v:3
    .A(a),
    .B(4'b0011),
    .Y(\\$n5 )
  );
  assign y = \\$n5 ;
endmodule
"""


class InstanceTests(unittest.TestCase):
    def test_parse(self) -> None:
        found = ic.instances(WRITTEN)
        self.assertEqual(len(found), 1)
        kind, params, name, pins = found[0]
        self.assertEqual(kind, "$pow")
        self.assertIn(".A_WIDTH(4)", params)
        self.assertEqual(name, "\\$c21")
        self.assertEqual(pins, {"A": "a", "B": "4'b0011", "Y": "$N"})

    def test_equal_up_to_generated_names_and_open_pins(self) -> None:
        read = WRITTEN.replace("\\$n5 ", "\\$n77 ").replace(".B(4'b0011)", ".B(4'b0011)")
        self.assertEqual(ic.compare_instances(WRITTEN, read), "")
        open_pin = WRITTEN.replace(".A(a)", ".A(1'bx)")
        self.assertIn("changed", ic.compare_instances(WRITTEN, open_pin))

    def test_dangling_generated_net_is_an_open_pin(self) -> None:
        dangling = WRITTEN.replace("  assign y = \\$n5 ;\n", "  assign y = a;\n")
        found = ic.instances(dangling)
        self.assertNotIn("Y", found[0][3])
        self.assertEqual(ic.compare_instances(dangling, dangling.replace(".Y(\\$n5 )", ".Y()")), "")

    def test_planted_parameter_change_is_found(self) -> None:
        changed = WRITTEN.replace(".Y_WIDTH(4)", ".Y_WIDTH(5)")
        self.assertIn("instance \\$c21 of $pow changed", ic.compare_instances(WRITTEN, changed))

    def test_lost_instance_is_found(self) -> None:
        self.assertEqual(
            ic.compare_instances(WRITTEN, "module m; endmodule\n"),
            "1 instances written, 0 read back",
        )

    def test_comments_are_dropped(self) -> None:
        self.assertEqual(ic.strip_comments("a  // x.v:1\nb\n"), "a\nb\n")


class ExpectTests(unittest.TestCase):
    TEXT = "# comment\nword a.v unproven Yosys has no model of $pow\ngate b.v fail:read why not\n"

    def test_parse(self) -> None:
        table = ic.parse_expect(self.TEXT)
        self.assertEqual(table[("word", "a.v")][0], "unproven")
        self.assertEqual(table[("gate", "b.v")], ("fail:read", "why not"))

    def test_bad_lines(self) -> None:
        for text in ("word a.v unproven\n", "nolevel a.v unproven why\n", "word a.v bogus why\n"):
            with self.assertRaises(ic.ToolError):
                ic.parse_expect(text)

    def test_unlisted_failure_fails_and_ok_passes(self) -> None:
        self.assertEqual(ic.judge_expected(ic.Verdict("ok"), None), (True, "ok"))
        self.assertFalse(ic.judge_expected(ic.Verdict("fail:equivalent"), None)[0])
        self.assertFalse(ic.judge_expected(ic.Verdict("unproven"), None)[0])

    def test_listed_outcome_passes_and_stale_fails(self) -> None:
        listed = ("unproven", "no model")
        self.assertTrue(ic.judge_expected(ic.Verdict("unproven"), listed)[0])
        passed, word = ic.judge_expected(ic.Verdict("ok"), listed)
        self.assertFalse(passed)
        self.assertIn("STALE", word)
        self.assertFalse(ic.judge_expected(ic.Verdict("fail:read"), listed)[0])


class ScriptTests(unittest.TestCase):
    def test_level_script_joins_and_substitutes(self) -> None:
        text = "# c\nread_verilog @DESIGN@\nproc\n\nwrite_verilog @OUT@\n"
        got = ic.level_script(text, {"@DESIGN@": "d.v", "@OUT@": "o.v"})
        self.assertEqual(got, "read_verilog d.v; proc; write_verilog o.v")

    def test_lpm_template_gets_the_gold_write(self) -> None:
        got = ic.lpm_template("read_techlib @LIB@; read_verilog @DESIGN@; write_verilog @OUT@\n")
        self.assertIn("write_rtlil @IL@; write_verilog @OUT@", got)
        with self.assertRaises(ic.ToolError):
            ic.lpm_template("read_verilog @DESIGN@\n")

    def test_the_level_scripts_exist_and_write_both_files(self) -> None:
        for level in ("word", "gate", "lut"):
            text = (ic.ROOT / "tests" / "icells" / f"{level}.script").read_text()
            self.assertIn("@DESIGN@", text)
            self.assertIn("@OUT@", text)
            self.assertIn("@IL@", text)

    def test_census(self) -> None:
        log = (
            "odin3: info: stats: module t: cell $dff 3\nodin3: info: stats: module t: cell $and 2\n"
        )
        self.assertEqual(ic.census(log), {("t", "$dff"): 3, ("t", "$and"): 2})

    def test_read_back_script_by_level(self) -> None:
        cfg = ic.Config("o", "y", Path("s"), Path("lib.o3lib"), Path("l"), 1)
        word = ic.read_back_script(cfg, "word", Path("W.v"), (Path("R.il"), Path("W2.v")))
        self.assertIn("read_verilog -icells W.v; proc; opt_clean; stats;", word)
        self.assertTrue(word.endswith("write_rtlil R.il"))
        self.assertNotIn("read_techlib", word)
        gate = ic.read_back_script(cfg, "gate", Path("W.v"), (Path("R.blif"), Path("W2.v")))
        self.assertTrue(gate.endswith("lower; write_blif --vtr R.blif"))
        lpm = ic.read_back_script(cfg, "lpm", Path("W.v"), (None, Path("W3.v")))
        self.assertTrue(lpm.startswith("read_techlib lib.o3lib; read_verilog -icells"))
        self.assertTrue(lpm.endswith("write_verilog W3.v"))


class MainTests(unittest.TestCase):
    def run_with(self, verdict: ic.Verdict, extra: list[str] | None = None) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            design = Path(tmp) / "d.v"
            design.write_text("module d; endmodule\n")
            with mock.patch.object(ic, "run_one", return_value=verdict):
                code, out, _ = run_main(
                    ic.main, ["--level", "word", "--work", tmp, *(extra or []), str(design)]
                )
        return code, out

    def test_ok(self) -> None:
        code, out = self.run_with(ic.Verdict("ok", "text fixpoint"))
        self.assertEqual(code, ic.EXIT_OK)
        self.assertIn("word d.v: ok (text fixpoint)", out)
        self.assertIn("compared 1 of 1 (0 unproven, 0 excepted)", out)

    def test_leg_failure_is_exit_1(self) -> None:
        code, out = self.run_with(ic.Verdict("fail:instances", "instance x changed"))
        self.assertEqual(code, ic.EXIT_FAIL)
        self.assertIn("fail:instances (instance x changed)", out)

    def test_unlisted_unproven_is_exit_4(self) -> None:
        code, _ = self.run_with(ic.Verdict("unproven", "size"))
        self.assertEqual(code, ic.EXIT_UNPROVEN)

    def test_listed_unproven_is_exit_0(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            expect = Path(tmp) / "expect.txt"
            expect.write_text("word d.v unproven no SAT model\n")
            code, out = self.run_with(ic.Verdict("unproven", "x"), ["--expect", str(expect)])
        self.assertEqual(code, ic.EXIT_OK)
        self.assertIn("excepted (unproven: no SAT model)", out)
        self.assertIn("1 excepted", out)

    def test_unknown_level_is_a_usage_error(self) -> None:
        code, _, err = run_main(ic.main, ["--level", "nope", "x.v"])
        self.assertEqual(code, ic.EXIT_ERROR)
        self.assertIn("unknown level", err)


if __name__ == "__main__":
    unittest.main()
