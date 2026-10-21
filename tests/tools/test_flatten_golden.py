"""Tests for tools/flatten-check/flatten_golden.py (3A spec 14.2): module extraction, the format
list of a fixture, the script, and the comparison with a planted difference."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import flatten_golden as fg

IL = "module \\top\n  wire \\a\nend\nmodule \\mid\nend\n"
BLIF = ".model top\n.inputs a\n.end\n\n.model mid\n.end\n"
VLOG = "module top (  // x.v:1\n  input a\n);\nendmodule\n\nmodule mid (\n);\nendmodule\n"


class TopPartTests(unittest.TestCase):
    def test_extracts_the_top_module_only(self) -> None:
        self.assertEqual(fg.top_part(IL, "il", "top"), "module \\top\n  wire \\a\nend\n")
        self.assertEqual(fg.top_part(BLIF, "blif", "top"), ".model top\n.inputs a\n.end\n")
        self.assertEqual(
            fg.top_part(VLOG, "v", "top"), "module top (  // x.v:1\n  input a\n);\nendmodule\n"
        )

    def test_absent_module(self) -> None:
        self.assertEqual(fg.top_part(IL, "il", "nope"), "")


class FormatTests(unittest.TestCase):
    def test_defaults_and_tag(self) -> None:
        self.assertEqual(fg.formats(Path("tests/flatten/hier.v")), ("il", "blif", "v"))
        self.assertEqual(fg.formats(Path("tests/flatten/nested.blif")), ("blif", "v"))
        self.assertEqual(fg.formats(Path("tests/flatten/mem.v")), ("il", "v"))

    def test_script(self) -> None:
        text = fg.script(Path("tests/flatten/hier.v"), "top", Path("/w/one"))
        self.assertIn("hierarchy --top top; proc; flatten; check;", text)
        self.assertIn("write_rtlil /w/one.il; write_blif /w/one.blif; write_verilog /w/one.v", text)
        blif = fg.script(Path("tests/flatten/nested.blif"), "top", Path("/w/one"))
        self.assertTrue(blif.startswith("read_blif tests/flatten/nested.blif; flatten; check;"))
        self.assertNotIn("write_rtlil", blif)


class CheckOneTests(unittest.TestCase):
    def _check(self, runs: list[dict[str, str] | str], expected: str | None) -> list[str] | str:
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(
            fg, "ROOT", Path(tmp)
        ), mock.patch.object(fg, "run_script", side_effect=runs):
            (Path(tmp) / "tests/flatten").mkdir(parents=True)
            (Path(tmp) / "tests/flatten/x.blif").write_text("# no tag\n")
            exp = Path(tmp) / fg.EXPECTED
            exp.mkdir(parents=True)
            if expected is not None:
                (exp / "x.blif").write_text(expected)
                (exp / "x.v").write_text(fg.top_part(VLOG, "v", "top"))
            return fg.check_one("odin3", Path("tests/flatten/x.blif"), "top", False)

    def test_equal(self) -> None:
        same = {"blif": BLIF, "v": VLOG}
        self.assertEqual(self._check([same, same], fg.top_part(BLIF, "blif", "top")), [])

    def test_planted_difference(self) -> None:
        same = {"blif": BLIF, "v": VLOG}
        problems = self._check([same, same], ".model top\n.inputs b\n.end\n")
        self.assertEqual(problems, ["blif: differs from x.blif"])

    def test_two_runs_differ(self) -> None:
        one = {"blif": BLIF, "v": VLOG}
        two = {"blif": BLIF + "# x\n", "v": VLOG}
        problems = self._check([one, two], fg.top_part(BLIF, "blif", "top"))
        self.assertEqual(problems, ["blif: two runs differ"])

    def test_missing_expected_file_and_run_error(self) -> None:
        same = {"blif": BLIF, "v": VLOG}
        problems = self._check([same, same], None)
        self.assertEqual(len(problems), 2)
        self.assertIn("no expected file", problems[0])
        self.assertEqual(self._check(["odin3 exit 1: boom", same], None), "odin3 exit 1: boom")


if __name__ == "__main__":
    unittest.main()
