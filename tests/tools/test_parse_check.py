"""Tests for odin3-parse-check's command line (2C spec §7.2, plan Task 9): exit codes, the golden
(--write, a refused hand-written golden, a seeded difference) and --dump. The binary comes from
ODIN3_PARSE_CHECK (set by CTest); the tests skip without it.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import helpers

TOOL = os.environ.get("ODIN3_PARSE_CHECK")
COVERAGE = helpers.REPO_ROOT / "tests" / "verilog" / "coverage"
ROW = "M1_nonansi_ports"
GENERATED = "# generated: odin3-parse-check --write\n"


@unittest.skipIf(not TOOL, "ODIN3_PARSE_CHECK not set (run through CTest)")
class ParseCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        shutil.copy(COVERAGE / f"{ROW}.v", self.dir)
        shutil.copy(COVERAGE / f"{ROW}.ast", self.dir)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_tool(self, *args: str) -> subprocess.CompletedProcess[str]:
        assert TOOL is not None
        return subprocess.run(
            [TOOL, *args], capture_output=True, text=True, timeout=120, check=False
        )

    def test_a_row_and_its_hand_golden_pass_from_another_directory(self) -> None:
        proc = self.run_tool(str(self.dir / f"{ROW}.v"))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("1 files, 0 failed", proc.stderr)

    def test_usage_errors_exit_2(self) -> None:
        self.assertEqual(self.run_tool().returncode, 2)
        self.assertEqual(self.run_tool("--bogus", str(self.dir / f"{ROW}.v")).returncode, 2)

    def test_missing_file_exits_2(self) -> None:
        proc = self.run_tool(str(self.dir / "nope.v"))
        self.assertEqual(proc.returncode, 2)
        self.assertIn("cannot read", proc.stderr)

    def test_seeded_dump_difference_exits_1(self) -> None:
        golden = self.dir / f"{ROW}.ast"
        golden.write_text(golden.read_text().replace(f"{ROW}.v:3:9", f"{ROW}.v:3:10", 1))
        proc = self.run_tool(str(self.dir / f"{ROW}.v"))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("the dump differs from the golden", proc.stderr)

    def test_write_refuses_a_hand_written_golden(self) -> None:
        golden = self.dir / f"{ROW}.ast"
        before = golden.read_text()
        proc = self.run_tool("--write", str(self.dir / f"{ROW}.v"))
        self.assertEqual(proc.returncode, 2)
        self.assertIn("refusing to overwrite a hand-written golden", proc.stderr)
        self.assertEqual(golden.read_text(), before)

    def test_write_then_compare(self) -> None:
        golden = self.dir / f"{ROW}.ast"
        hand = golden.read_text()
        golden.unlink()
        self.assertEqual(self.run_tool("--write", str(self.dir / f"{ROW}.v")).returncode, 0)
        written = golden.read_text()
        self.assertTrue(written.startswith(GENERATED))
        self.assertEqual(written[len(GENERATED) :], hand.split("\n", 1)[1])
        self.assertEqual(self.run_tool(str(self.dir / f"{ROW}.v")).returncode, 0)

    def test_dump_prints_the_dump(self) -> None:
        proc = self.run_tool("--dump", str(self.dir / f"{ROW}.v"))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout, (self.dir / f"{ROW}.ast").read_text().split("\n", 1)[1])

    def test_a_syntax_error_exits_1(self) -> None:
        bad = self.dir / "bad.v"
        bad.write_text("module m (a b);\nendmodule\n")
        proc = self.run_tool(str(bad))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("bad.v:1:13: error: syntax:", proc.stderr)

    def test_any_failure_fails_the_run(self) -> None:
        bad = self.dir / "bad.v"
        bad.write_text("module m (a b);\nendmodule\n")
        proc = self.run_tool(str(self.dir / f"{ROW}.v"), str(bad))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("2 files, 1 failed", proc.stderr)


if __name__ == "__main__":
    unittest.main()
