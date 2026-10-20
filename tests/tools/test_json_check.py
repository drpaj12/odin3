"""Tests for tools/json-check/json_check.py (5.3 spec sections 6 and 9): the R-2 normalisation, the
first differing line, the expect file, the legs in order and their outcomes with the Odin III and
equivalence runs replaced by fixed ones, and (with the build's odin3 and Yosys 0.55) a mutated
read-back end to end, one per leg.
"""

from __future__ import annotations

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


class ExpectTests(unittest.TestCase):
    def test_parse_both_kinds_of_level(self) -> None:
        table = jc.parse_expect("# c\nword a.v unproven why\ny-gate b.json fail:read $x\n")
        self.assertEqual(table[("word", "a.v")][0], "unproven")
        self.assertEqual(table[("y-gate", "b.json")], ("fail:read", "$x"))

    def test_bad_lines(self) -> None:
        for text in ("word a.v unproven\n", "lpm a.v unproven why\n", "word a.v bogus why\n"):
            with self.assertRaises(jc.ToolError):
                jc.parse_expect(text)


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
            expect.write_text("word a.v error:script no cell type\n")
            args = ["--odin3", "o", "--expect", str(expect)]
            code, out, _ = self.run_with([jc.Row("error:script", "m")], args)
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

    DESIGN = jc.ROOT / "tests" / "micro" / "verilog" / "micro" / "ff.v"

    def run_mutated(self, mode: str, level: str = "word") -> tuple[int, str]:
        cli, yosys = os.environ.get("ODIN3_CLI"), os.environ.get("ODIN3_YOSYS055")
        if not cli or not yosys or not Path(yosys).exists():
            self.skipTest("needs ODIN3_CLI and ODIN3_YOSYS055")
        fake = Path(__file__).resolve().parent / "fixtures" / "json_mutating_odin3"
        env = {"JSON_MUTATE": mode, "ODIN3_CLI": cli}
        with mock.patch.dict(os.environ, env):
            code, out, _ = run_main(
                jc.main,
                ["--level", level, "--odin3", str(fake), "--yosys", yosys, str(self.DESIGN)],
            )
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


if __name__ == "__main__":
    unittest.main()
