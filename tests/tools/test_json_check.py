"""Tests for tools/json-check/json_check.py (5.3 spec sections 6 and 9): the R-2 normalisation, the
first differing line, the expect file, the legs in order and their outcomes with the Odin III and
equivalence runs replaced by fixed ones, and (with the build's odin3 and Yosys 0.55) a mutated
read-back end to end, one per leg.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import tempfile
import unittest
from collections.abc import Callable, Sequence
from pathlib import Path
from subprocess import CompletedProcess
from unittest import mock

import icells_check as ic
import json_check as jc

from .helpers import run_main

FILE1 = """{
  "modules": {
    "top": {
      "ports": { "a": { "direction": "input", "bits": [ 2 ] } },
      "cells": {
        "$c12": { "type": "$not", "connections": { "A": [ 2 ], "Y": [ 3 ] } }
      },
      "netnames": {
        "a": { "hide_name": 0, "bits": [ 2 ] },
        "$n34": { "hide_name": 1, "bits": [ 3 ] }
      }
    }
  }
}
"""


class NormaliseTests(unittest.TestCase):
    def test_generated_names_and_bit_numbers_are_normalised(self) -> None:
        other = FILE1.replace("$c12", "$c99").replace("$n34", "$n7").replace("[ 3 ]", "[ 9 ]")
        other = other.replace("[ 2 ]", "[ 5 ]")
        self.assertNotEqual(FILE1, other)
        self.assertEqual(jc.normalise_json(FILE1), jc.normalise_json(other))
        self.assertEqual(jc.diff_note(FILE1, FILE1), "same")
        self.assertEqual(jc.diff_note(FILE1, other), "normalised")

    def test_numbering_in_another_order_is_the_same_wiring(self) -> None:
        other = FILE1.replace("[ 2 ]", "[ 8 ]").replace("[ 3 ]", "[ 5 ]")
        self.assertEqual(jc.normalise_json(FILE1), jc.normalise_json(other))
        swapped = FILE1.replace("[ 2 ]", "[ 9 ]").replace("[ 3 ]", "[ 2 ]")
        swapped = swapped.replace("[ 9 ]", "[ 3 ]")
        self.assertEqual(jc.normalise_json(FILE1), jc.normalise_json(swapped))

    def test_a_real_difference_survives_normalisation(self) -> None:
        other = FILE1.replace('"$not"', '"$neg"')
        note = jc.diff_note(FILE1, other)
        self.assertIn("$not", note)
        self.assertIn("$neg", note)
        self.assertTrue(note.startswith("line "))

    def test_a_wiring_change_survives_normalisation(self) -> None:
        other = FILE1.replace('"Y": [ 3 ]', '"Y": [ 2 ]')
        self.assertNotEqual(jc.diff_note(FILE1, other), "normalised")

    def test_first_difference(self) -> None:
        self.assertEqual(jc.first_difference("a\nb\n", "a\nb\n"), "")
        self.assertEqual(jc.first_difference("a\nb\n", "a\nc\n"), "line 2: b | c")
        self.assertIn("<end of file>", jc.first_difference("a\n", "a\nb\n"))

    def test_counts(self) -> None:
        self.assertEqual(jc.json_counts(FILE1), (1, 2, 2))
        self.assertEqual(json.loads(FILE1)["modules"]["top"]["cells"]["$c12"]["type"], "$not")


class PlainRtlilTests(unittest.TestCase):
    def test_src_module_attributes_and_a_32_bit_top_are_made_plain(self) -> None:
        top32 = "attribute \\top 32'00000000000000000000000000000001\n"
        text = (
            top32
            + 'attribute \\src "a.v:1.1-2.2"\nattribute \\hdlname "b"\nattribute \\blackbox 1\n'
            + 'module \\m\n  attribute \\src "a.v:3.1-3.2"\n  wire \\w\nend\n'
        )
        want = "attribute \\top 1\nattribute \\blackbox 1\nmodule \\m\n  wire \\w\nend\n"
        self.assertEqual(jc.plain_rtlil(text), want)
        self.assertEqual(jc.plain_rtlil(want), want)


class ExpectTests(unittest.TestCase):
    def test_parse_both_kinds_of_level(self) -> None:
        table = jc.parse_expect("# c\nword a.v unproven why\ny-gate b.json fail:read $x\n")
        self.assertEqual(table[("word", "a.v")][0], "unproven")
        self.assertEqual(table[("y-gate", "b.json")], ("fail:read", "$x"))

    def test_bad_lines(self) -> None:
        for text in ("word a.v unproven\n", "lpm a.v unproven why\n", "word a.v bogus why\n"):
            with self.assertRaises(jc.ToolError):
                jc.parse_expect(text)


class JudgeListedTests(unittest.TestCase):
    """A listed failure matches only when the observed text holds the entry's key."""

    CELL = ("fail:read", "Yosys cell type $_SDFF_PP0_ is not registered")

    def test_matching_key_is_excepted(self) -> None:
        text = "read_json: f.json:9: cell 'x' has type '$_SDFF_PP0_', which is not registered"
        passed, word = jc.judge_listed(ic.Verdict("fail:read", text), self.CELL)
        self.assertTrue(passed)
        self.assertIn("excepted", word)

    def test_other_read_failure_is_unexpected(self) -> None:
        text = "read_json: f.json:9: cell 'x' has type '$_DFF_PN0_', which is not registered"
        passed, word = jc.judge_listed(ic.Verdict("fail:read", text), self.CELL)
        self.assertFalse(passed)
        self.assertIn("UNEXPECTED", word)

    def test_key_is_not_a_prefix_match(self) -> None:
        entry = ("fail:read", "positional parameter ($1)")
        passed, _ = jc.judge_listed(ic.Verdict("fail:read", "parameter $10 unknown"), entry)
        self.assertFalse(passed)

    def test_backquoted_phrase_is_a_key(self) -> None:
        entry = ("fail:read", "refuses the `level-sensitive latch`")
        good = ic.Verdict("fail:read", "write_blif: level-sensitive latch; VPR")
        self.assertTrue(jc.judge_listed(good, entry)[0])
        self.assertFalse(jc.judge_listed(ic.Verdict("fail:read", "bad file"), entry)[0])

    def test_entry_without_key_is_refused(self) -> None:
        passed, word = jc.judge_listed(ic.Verdict("fail:read", "x"), ("fail:read", "no key"))
        self.assertFalse(passed)
        self.assertIn("names no", word)

    def test_unproven_matches_on_outcome(self) -> None:
        passed, _ = jc.judge_listed(ic.Verdict("unproven", "t"), ("unproven", "why"))
        self.assertTrue(passed)

    def test_unlisted_and_stale_as_before(self) -> None:
        self.assertEqual(jc.judge_listed(ic.Verdict("ok"), None), (True, "ok"))
        self.assertFalse(jc.judge_listed(ic.Verdict("ok"), self.CELL)[0])
        self.assertFalse(jc.judge_listed(ic.Verdict("fail:read", "$_SDFF_PP0_"), None)[0])


    def test_unproven_entry_not_stale_without_equivalence(self) -> None:
        entry = ("unproven", "no plain flip-flop form")
        skipped = jc.Row("ok", "equivalence not run: no Yosys 0.55", equivalence=False)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(jc.report("y-gate", Path("d.json"), skipped, entry)[0])
            self.assertFalse(jc.report("y-gate", Path("d.json"), jc.Row("ok"), entry)[0])

class FirstErrorTests(unittest.TestCase):
    def test_marker_line_beats_earlier_mention(self) -> None:
        err = "odin3: warning: no error here\nodin3: error: read_json: the real one\n"
        done = CompletedProcess([], 1, "", err)
        self.assertEqual(jc.first_error(done), "read_json: the real one")

    def test_falls_back_to_any_error_line_then_first_line(self) -> None:
        done = CompletedProcess([], 1, "", "a\nan error x\n")
        self.assertEqual(jc.first_error(done), "an error x")
        self.assertEqual(jc.first_error(CompletedProcess([], 1, "", "a\nb\n")), "a")


def fake_odin3(
    content: dict[str, str], fail_read: bool = False
) -> Callable[..., CompletedProcess[str]]:
    """An ``ic.odin3_run`` that writes the files of a round trip: the level script writes F1 (and
    the gold), a ``read_json`` run writes the file after it named by ``write_json``."""

    def run(
        odin3: str, script: str, work: Path, flags: Sequence[str] = ()
    ) -> CompletedProcess[str]:
        del odin3, flags
        if "read_json" in script:
            if fail_read:
                return CompletedProcess([], 1, "", "odin3: error: bad file\n")
            target = script.split("write_json ")[1].split(";")[0].strip()
            name = Path(target).name
            Path(target).write_text(content[name], encoding="utf-8")
            if "write_rtlil" in script:
                Path(script.split("write_rtlil ")[1].strip()).write_text("rtlil\n")
            if "write_blif" in script:
                Path(script.split("write_blif --vtr ")[1].strip()).write_text("blif\n")
        else:
            (work / "F1.json").write_text(content["F1.json"], encoding="utf-8")
            (work / "G.il").write_text("gold\n", encoding="utf-8")
            (work / "G.blif").write_text("gold\n", encoding="utf-8")
        return CompletedProcess([], 0, "", "")

    return run


class LegTests(unittest.TestCase):
    """One design at one level with every child replaced."""

    def run_one(
        self,
        level: str,
        content: dict[str, str],
        equiv: ic.Verdict | None = None,
        fail_read: bool = False,
    ) -> jc.Row:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            cfg = jc.Config("odin3", "yosys", jc.ROOT / "tests" / "json", 1)
            cfg.techlibs = {}
            mock.patch.object(ic, "odin3_run", fake_odin3(content, fail_read)).start()
            mock.patch.object(
                jc, "judge_equivalent", return_value=equiv or ic.Verdict("ok")
            ).start()
            identical = jc.Row("ok", "", "identical", (1, 2, 2), "same")
            mock.patch.object(jc, "compare_row", return_value=identical).start()
            self.addCleanup(mock.patch.stopall)
            return jc.run_design(cfg, level, Path("d.v"), work)

    def test_all_legs_pass(self) -> None:
        content = {"F1.json": FILE1, "F2.json": FILE1, "F3.json": FILE1}
        row = self.run_one("word", content)
        self.assertEqual((row.outcome, row.diff, row.counts), ("ok", "same", (1, 2, 2)))
        row = self.run_one("lut", content)
        self.assertEqual((row.outcome, row.kind), ("ok", "identical"))

    def test_r1_failure_names_the_first_differing_line(self) -> None:
        file3 = FILE1.replace('"$not"', '"$neg"')
        row = self.run_one("word", {"F1.json": FILE1, "F2.json": FILE1, "F3.json": file3})
        self.assertEqual(row.outcome, "fail:fixpoint")
        self.assertIn("file 2 differs from file 3", row.reason)
        self.assertIn('"type": "$neg"', row.reason)

    def test_a_failing_read_is_fail_read(self) -> None:
        row = self.run_one("word", {"F1.json": FILE1}, fail_read=True)
        self.assertEqual(row.outcome, "fail:read")
        self.assertIn("bad file", row.reason)

    def test_an_inequivalent_read_back_fails_the_equivalence_leg(self) -> None:
        content = {"F1.json": FILE1, "F2.json": FILE1, "F3.json": FILE1}
        row = self.run_one("gate", content, ic.Verdict("fail:equivalent", "outputs differ"))
        self.assertEqual((row.outcome, row.reason), ("fail:equivalent", "outputs differ"))

    def test_r2_difference_is_reported_not_failed(self) -> None:
        file2 = FILE1.replace('"$not"', '"$neg"')
        row = self.run_one("word", {"F1.json": FILE1, "F2.json": file2, "F3.json": file2})
        self.assertEqual(row.outcome, "ok")
        self.assertTrue(row.diff.startswith("line "))


class ScriptTests(unittest.TestCase):
    def test_the_level_scripts_write_the_gold_and_file_1(self) -> None:
        for level in jc.LEVELS:
            text = (jc.ROOT / "tests" / "json" / f"{level}.script").read_text()
            for word in ("@DESIGN@", "@IL@", "@JSON@", "write_json"):
                self.assertIn(word, text, level)

    def test_read_back_script(self) -> None:
        text = jc.read_back_script("", Path("F1.json"), (Path("F2.json"), "write_rtlil R.il"))
        self.assertEqual(text, "read_json F1.json; write_json F2.json; write_rtlil R.il")
        text = jc.read_back_script("read_techlib t", Path("F2.json"), (Path("F3.json"), ""))
        self.assertEqual(text, "read_techlib t; read_json F2.json; write_json F3.json")


class GoldTests(unittest.TestCase):
    """The read-back writer, and Yosys's gold for the gate-level flip-flops with reset or enable."""

    def test_read_back_writer_lowers_only_for_gate_ff_types(self) -> None:
        self.assertEqual(jc.read_back_writer("word", Path("R.il")), "write_rtlil R.il")
        plain = '{"type": "$_DFF_P_"} {"type": "$_AND_"}'
        for level in ("gate", "lut"):
            self.assertEqual(
                jc.read_back_writer(level, Path("R.blif"), plain), "write_blif --vtr R.blif"
            )
            for kind in ("$_SDFF_PP0_", "$_DFFE_PN1P_", "$_DFFE_NP_", "$_DFF_NN0_"):
                self.assertEqual(
                    jc.read_back_writer(level, Path("R.blif"), f'{{"type": "{kind}"}}'),
                    "lower; write_blif --vtr R.blif",
                    kind,
                )

    def test_gold_is_legalised_only_for_the_new_families(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for kind, legalised in (
                ("$_SDFF_PP0_", True),
                ("$_DFFE_PP_", True),
                ("$_DFF_PN1_", True),
                ("$_SDFFCE_NN1P_", True),
                ("$_DFF_P_", False),
                ("$_DFF_N_", False),
                ("$_DLATCH_P_", False),
            ):
                fixture = Path(tmp) / "f.json"
                fixture.write_text(f'{{"cells": {{"c": {{"type": "{kind}"}}}}}}', encoding="utf-8")
                text = jc.gold_commands("gate", fixture, Path("g.blif"))
                self.assertEqual("dfflegalize" in text, legalised, kind)
                self.assertTrue(text.endswith("write_blif g.blif"), text)
            word = jc.gold_commands("word", fixture, Path("g.il"))
            self.assertEqual(word, f"read_json {fixture}; write_rtlil g.il")


class MainTests(unittest.TestCase):
    def run_with(
        self, rows: Sequence[jc.Row], extra: Sequence[str] = ()
    ) -> tuple[int, str, str]:
        queue = list(rows)

        def run(cfg: jc.Config, level: str, design: Path, work: Path) -> jc.Row:
            del cfg, level, design, work
            return queue.pop(0)

        with mock.patch.object(jc, "run_design", side_effect=run):
            return run_main(jc.main, ["--level", "word", *extra, "a.v"])

    def test_unlisted_failure_is_exit_1(self) -> None:
        code, out, _ = self.run_with([jc.Row("fail:fixpoint", "x")], ["--odin3", "o"])
        self.assertEqual(code, jc.EXIT_FAIL)
        self.assertIn("fail:fixpoint", out)

    def test_nothing_compared_fails(self) -> None:
        code, _, err = self.run_with([jc.Row("error:script", "x")], ["--odin3", "o"])
        self.assertEqual(code, jc.EXIT_FAIL)
        self.assertIn("nothing was compared", err)

    def test_listed_exception_and_stale_entry(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            expect = Path(tmp) / "expect.txt"
            expect.write_text("word a.v error:script no cell `type`\n")
            args = ["--odin3", "o", "--expect", str(expect)]
            code, out, _ = self.run_with([jc.Row("error:script", "no cell type")], args)
            self.assertIn("excepted", out)
            self.assertEqual(code, jc.EXIT_FAIL)  # the only design: nothing compared
            code, out, _ = self.run_with([jc.Row("ok", "", "identical")], args)
            self.assertEqual(code, jc.EXIT_FAIL)
            self.assertIn("STALE", out)

    def test_report_row_and_summary(self) -> None:
        row = jc.Row("ok", "", "equivalent-only", (3, 4, 5), "same")
        code, out, _ = self.run_with([row], ["--odin3", "o"])
        self.assertEqual(code, jc.EXIT_OK)
        self.assertIn("word a.v: ok equivalent-only cells 3 wires 4 nets 5", out)
        self.assertIn("compared 1 of 1", out)
        self.assertIn("equivalent-only 1", out)

    def test_unexplained_file_difference_fails(self) -> None:
        row = jc.Row("ok", "", "identical", (1, 1, 1), "line 3: a | b")
        code, out, err = self.run_with([row], ["--odin3", "o"])
        self.assertEqual(code, jc.EXIT_FAIL)
        self.assertIn("after it in 1", out)
        self.assertIn("after normalisation", err)

    def test_normalised_difference_passes(self) -> None:
        row = jc.Row("ok", "", "identical", (1, 1, 1), "normalised")
        code, _, _ = self.run_with([row], ["--odin3", "o"])
        self.assertEqual(code, jc.EXIT_OK)

    def test_missing_yosys_is_a_usage_error(self) -> None:
        code, _, err = run_main(
            jc.main, ["--level", "word", "--yosys", "/nonexistent/yosys", "a.v"]
        )
        self.assertEqual(code, jc.EXIT_ERROR)
        self.assertIn("names no file", err)

    def test_unknown_level_is_a_usage_error(self) -> None:
        code, _, err = run_main(jc.main, ["--level", "nope", "x.v"])
        self.assertEqual(code, jc.EXIT_ERROR)
        self.assertIn("unknown level", err)

    def test_no_designs_is_a_usage_error(self) -> None:
        code, _, err = run_main(jc.main, ["--level", "word"])
        self.assertEqual(code, jc.EXIT_ERROR)
        self.assertIn("no designs", err)


class MutatedReadBackTests(unittest.TestCase):
    """End to end (needs the build's odin3 in ODIN3_CLI and Yosys 0.55 in ODIN3_YOSYS055): a
    round trip damaged after Odin III wrote it fails the leg that watches that file."""

    MICRO = jc.ROOT / "tests" / "micro" / "verilog" / "micro"

    def run_mutated(self, mode: str, level: str = "word", design: str = "ff.v") -> tuple[int, str]:
        cli, yosys = os.environ.get("ODIN3_CLI"), os.environ.get("ODIN3_YOSYS055")
        if not cli or not yosys or not Path(yosys).exists():
            self.skipTest("needs ODIN3_CLI and ODIN3_YOSYS055")
        fake = Path(__file__).resolve().parent / "fixtures" / "json_mutating_odin3"
        env = {"JSON_MUTATE": mode, "ODIN3_CLI": cli}
        with mock.patch.dict(os.environ, env):
            argv = ["--level", level, "--odin3", str(fake), "--yosys", yosys]
            code, out, _ = run_main(jc.main, [*argv, str(self.MICRO / design)])
        return code, out

    def test_unmutated_passes(self) -> None:
        code, out = self.run_mutated("")
        self.assertEqual(code, jc.EXIT_OK, out)

    def test_read_leg(self) -> None:
        code, out = self.run_mutated("read")
        self.assertEqual(code, jc.EXIT_FAIL)
        self.assertIn("fail:read", out)

    def test_fixpoint_leg(self) -> None:
        code, out = self.run_mutated("fixpoint")
        self.assertEqual(code, jc.EXIT_FAIL)
        self.assertIn("fail:fixpoint", out)

    def test_equivalent_leg_word(self) -> None:
        code, out = self.run_mutated("equivalent")
        self.assertEqual(code, jc.EXIT_FAIL)
        self.assertIn("fail:equivalent", out)

    def test_equivalent_leg_lut(self) -> None:
        abc = os.environ.get("ODIN3_ABC")
        if not abc:
            self.skipTest("needs ODIN3_ABC")
        code, out = self.run_mutated("equivalent", "lut", "bm_DL_2_1_mux.v")
        self.assertEqual(code, jc.EXIT_FAIL)
        self.assertIn("fail:equivalent", out)


if __name__ == "__main__":
    unittest.main()
