"""tb-compare: identical designs agree, a mutant is reported at its first differing cycle."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLI = os.environ.get("ODIN3_CLI", "")
MICRO = ROOT / "tests" / "tb" / "micro"
TOOL = ROOT / "tools" / "tb-compare" / "tb-compare"

MUTANT = (MICRO / "counter.v").read_text().replace("q + 1", "q + 2")


@unittest.skipUnless(CLI, "ODIN3_CLI not set")
class TbCompareTest(unittest.TestCase):
    def run_tool(self, b_file: Path) -> "subprocess.CompletedProcess[str]":
        script = "read_verilog {}; proc; opt; lower"
        return subprocess.run(
            [sys.executable, str(TOOL), "--odin3", CLI, "--tb", str(MICRO / "counter_tb.v"),
             "--a", script.format(MICRO / "counter.v"), "--b", script.format(b_file)],
            capture_output=True, text=True, check=False)

    def test_identical(self) -> None:
        proc = self.run_tool(MICRO / "counter.v")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_mutant_first_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "counter.v"
            bad.write_text(MUTANT)
            proc = self.run_tool(bad)
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("MISMATCH line 1 (time/cycle t=26)", proc.stdout)

    def test_path_with_spaces(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            spaced = Path(tmp) / "a dir"
            spaced.mkdir()
            tb = spaced / "counter tb.v"
            tb.write_text((MICRO / "counter_tb.v").read_text())
            script = "read_verilog {}; proc; opt; lower".format(MICRO / "counter.v")
            proc = subprocess.run(
                [sys.executable, str(TOOL), "--odin3", CLI, "--tb", str(tb),
                 "--a", script, "--b", script],
                capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_empty_outputs_are_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tb = Path(tmp) / "quiet_tb.v"
            tb.write_text("module quiet_tb;\n  initial #1 $finish;\nendmodule\n")
            script = "read_verilog {}; proc; opt; lower".format(MICRO / "counter.v")
            proc = subprocess.run(
                [sys.executable, str(TOOL), "--odin3", CLI, "--tb", str(tb),
                 "--a", script, "--b", script],
                capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("nothing was compared", proc.stderr)


if __name__ == "__main__":
    unittest.main()
