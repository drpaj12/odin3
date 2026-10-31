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


if __name__ == "__main__":
    unittest.main()
