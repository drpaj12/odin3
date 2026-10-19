"""Tests for tools/icells-check/icells_check.py (5.2 spec sections 7 to 9): the instance parser and
comparison (a planted parameter change is found), the expect file (stale and wrong entries), the
script text of a level, the census, the exit codes and the per-design report, with the Odin III
runs replaced by fixed verdicts.
"""

from __future__ import annotations

import os
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
        cfg.techlibs = {"hard.v": ["read_techlib vtr.o3lib", "read_verilog --lib adder.v"]}
        word = ic.read_back_script(
            cfg,
            "word",
            Path("W.v"),
            ic.ReadOut(Path("R.il"), Path("W2.v"), Path("S2.il")),
            Path("d.v"),
        )
        self.assertIn("read_verilog -icells W.v; proc; opt_clean; stats; write_rtlil S2.il;", word)
        self.assertTrue(word.endswith("write_verilog W2.v; write_rtlil R.il"))
        self.assertNotIn("read_techlib", word)
        gate = ic.read_back_script(
            cfg,
            "gate",
            Path("W.v"),
            ic.ReadOut(Path("R.blif"), Path("W2.v"), Path("S2.il")),
            Path("d.v"),
        )
        self.assertTrue(gate.endswith("; lower; write_blif --vtr R.blif"))
        lpm = ic.read_back_script(
            cfg, "lpm", Path("W.v"), ic.ReadOut(None, Path("W3.v"), Path("S3.il")), Path("hard.v")
        )
        self.assertTrue(
            lpm.startswith(
                "read_techlib lib.o3lib; read_techlib vtr.o3lib; read_verilog --lib adder.v; "
                "read_verilog -icells"
            )
        )
        self.assertTrue(lpm.endswith("write_verilog W3.v"))
        text = ic.read_back_script(
            cfg, "insttext", Path("W.v"), ic.ReadOut(None, Path("W2.v"), Path("S2.il")), Path("d.v")
        )
        self.assertNotIn("opt_clean", text)
        self.assertNotIn("write_rtlil", text)

    def test_required_type_and_library_instances(self) -> None:
        self.assertEqual(ic.required_type("inst", Path("inst_dffsr.v")), "$dffsr")
        self.assertEqual(ic.required_type("insttext", Path("inst_memwr.v")), "$memwr")
        self.assertEqual(ic.required_type("word", Path("inst_dffsr.v")), "")
        text = "(* blackbox *)\nmodule lpm_x #(parameter a = 1) (input b);\nendmodule\n"
        text += (
            "module t;\n  lpm_x #(.a(2)) \\$c3 (\n    .b(1'b1)\n  );\n  lpm_x u ();\nendmodule\n"
        )
        self.assertEqual(ic.lib_instances(text), 2)

    def test_parse_techlibs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "t.txt"
            path.write_text("# c\na.v lib/x.o3lib\nb.v lib/y.o3lib soft.v\n")
            table = ic.parse_techlibs(path)
            self.assertEqual(table["a.v"], [f"read_techlib {ic.ROOT / 'lib/x.o3lib'}"])
            self.assertEqual(len(table["b.v"]), 2)
            self.assertIn("read_verilog --lib", table["b.v"][1])
            path.write_text("a.v\n")
            with self.assertRaises(ic.ToolError):
                ic.parse_techlibs(path)


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
        code, out = self.run_with(ic.Verdict("ok", "text fixpoint", 4))
        self.assertEqual(code, ic.EXIT_OK)
        self.assertIn("word d.v: ok (text fixpoint)", out)
        self.assertIn("compared 1 of 1 (0 unproven, 0 excepted); 4 $ instances", out)

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


IL = """module \\top
  wire width 4 input 1 \\d
  wire width 4 output 2 \\q
  cell $dff $c1
    parameter \\\\WIDTH 4
    connect \\D \\d
    connect \\Q \\q
  end
end
"""
CENSUS = {("top", "$dff"): 1}


class JudgeTextTests(unittest.TestCase):
    """The fixpoint and instance legs on files, and the require flags."""

    def verdict(
        self,
        census: dict[tuple[str, str], int],
        il3: str = IL,
        text: str = WRITTEN,
        flags: tuple[bool, bool] = (False, False),
        design: str = "d.v",
    ) -> ic.Verdict:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            paths = {name: work / name for name in ("W.v", "W2.v", "W3.v", "S2.il", "S3.il")}
            for name in ("W.v", "W2.v", "W3.v"):
                paths[name].write_text(text)
            paths["S2.il"].write_text(IL)
            paths["S3.il"].write_text(il3)
            cfg = ic.Config("o", "y", Path("s"), Path("l"), Path("p"), 1)
            cfg.require_dollar, cfg.require_lib = flags
            return ic.judge_text(cfg, ("word", Path(design)), paths, (census, census))

    def test_ok_counts_the_instances(self) -> None:
        verdict = self.verdict(CENSUS)
        self.assertEqual(verdict.outcome, "ok")
        self.assertEqual(verdict.instances, 1)

    def test_an_empty_census_fails(self) -> None:
        self.assertEqual(self.verdict({}).outcome, "fail:fixpoint")
        self.assertIn("no cell census", self.verdict({}).reason)

    def test_a_changed_parameter_in_generation_3_fails(self) -> None:
        verdict = self.verdict(CENSUS, il3=IL.replace("WIDTH 4", "WIDTH 5"))
        self.assertEqual(verdict.outcome, "fail:fixpoint")
        self.assertIn("parameter", verdict.reason)

    def test_an_empty_signature_fails(self) -> None:
        verdict = self.verdict(CENSUS, il3="")
        self.assertEqual(verdict.outcome, "fail:fixpoint")

    def test_a_design_without_dollar_instance_fails_when_required(self) -> None:
        plain = "module m (input a, output y);\n  assign y = a;\nendmodule\n"
        self.assertEqual(self.verdict(CENSUS, text=plain).outcome, "ok")
        verdict = self.verdict(CENSUS, text=plain, flags=(True, False))
        self.assertEqual(verdict.outcome, "fail:instances")
        self.assertEqual(
            self.verdict(CENSUS, text=plain, flags=(False, True)).outcome, "fail:instances"
        )

    def test_the_type_of_an_inst_file_must_be_present(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            paths = {n: work / n for n in ("W.v", "W2.v", "W3.v", "S2.il", "S3.il")}
            for name in ("W.v", "W2.v", "W3.v"):
                paths[name].write_text(WRITTEN)
            paths["S2.il"].write_text(IL)
            paths["S3.il"].write_text(IL)
            cfg = ic.Config("o", "y", Path("s"), Path("l"), Path("p"), 1)
            verdict = ic.judge_text(cfg, ("inst", Path("inst_dffsr.v")), paths, (CENSUS, CENSUS))
            self.assertEqual(verdict.outcome, "fail:instances")
            self.assertIn("$dffsr", verdict.reason)
            verdict = ic.judge_text(cfg, ("inst", Path("inst_pow.v")), paths, (CENSUS, CENSUS))
            self.assertEqual(verdict.outcome, "ok")


class MutatedReadBackTests(unittest.TestCase):
    """End to end (needs the build's odin3 in ODIN3_CLI and Yosys 0.55 in ODIN3_YOSYS055): a
    read-back mutated after Odin III wrote it fails the leg that watches that file (review L3)."""

    DESIGN = ic.ROOT / "tests" / "icells" / "inst" / "inst_dffsr.v"

    def run_mutated(self, mode: str) -> tuple[int, str]:
        cli, yosys = os.environ.get("ODIN3_CLI"), os.environ.get("ODIN3_YOSYS055")
        if not cli or not yosys or not Path(yosys).exists():
            self.skipTest("needs ODIN3_CLI and ODIN3_YOSYS055")
        fake = Path(__file__).resolve().parent / "fixtures" / "icells_mutating_odin3"
        env = {"ICELLS_MUTATE": mode, "ODIN3_CLI": cli}
        with mock.patch.dict(os.environ, env):
            code, out, _ = run_main(
                ic.main,
                ["--level", "inst", "--odin3", str(fake), "--yosys", yosys, str(self.DESIGN)],
            )
        return code, out

    def test_unmutated_passes(self) -> None:
        code, out = self.run_mutated("")
        self.assertEqual(code, ic.EXIT_OK, out)

    def test_read_leg(self) -> None:
        code, out = self.run_mutated("read")
        self.assertEqual(code, ic.EXIT_FAIL)
        self.assertIn("fail:read", out)

    def test_instances_leg(self) -> None:
        code, out = self.run_mutated("instances")
        self.assertEqual(code, ic.EXIT_FAIL)
        self.assertIn("fail:instances", out)

    def test_fixpoint_leg(self) -> None:
        code, out = self.run_mutated("fixpoint")
        self.assertEqual(code, ic.EXIT_FAIL)
        self.assertIn("fail:fixpoint", out)

    def test_equivalent_leg(self) -> None:
        code, out = self.run_mutated("equivalent")
        self.assertEqual(code, ic.EXIT_FAIL)
        self.assertIn("fail:equivalent", out)


if __name__ == "__main__":
    unittest.main()
