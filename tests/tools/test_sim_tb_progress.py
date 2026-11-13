"""``sim_tb`` progress lines (``--progress SEC``) and the warning when ``--max-time`` ends a run.
Needs ``ODIN3_CLI``."""

from __future__ import annotations

import os
import subprocess
import unittest
from pathlib import Path

ODIN3 = os.environ.get("ODIN3_CLI", "")
FIX = Path(__file__).resolve().parents[1] / "tb" / "progress"
FOREVER = str(FIX / "clk_forever.v")
FINISH = str(FIX / "finish.v")


def run(script: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([ODIN3, "-p", script], capture_output=True, text=True, check=False,
                          timeout=120)


@unittest.skipUnless(ODIN3 and Path(ODIN3).exists(), "odin3 (ODIN3_CLI) is needed")
class ProgressTest(unittest.TestCase):
    def test_no_progress_by_default(self) -> None:
        done = run(f"sim_tb --top t --max-time 3000000 {FOREVER}")
        self.assertEqual(0, done.returncode, done.stderr)
        self.assertNotIn("progress", done.stderr)

    def test_progress_lines(self) -> None:
        done = run(f"sim_tb --top t --max-time 8000000 --progress 1 {FOREVER}")
        self.assertEqual(0, done.returncode, done.stderr)
        lines = [ln for ln in done.stderr.splitlines() if "sim_tb: progress:" in ln]
        self.assertGreaterEqual(len(lines), 1, done.stderr)
        for ln in lines:
            self.assertIn("of 8000000", ln)
            self.assertIn("time steps", ln)
            self.assertIn("time units/s", ln)

    def test_bad_progress_value(self) -> None:
        done = run(f"sim_tb --top t --progress soon {FOREVER}")
        self.assertNotEqual(0, done.returncode)
        self.assertIn("--progress SEC", done.stderr)


@unittest.skipUnless(ODIN3 and Path(ODIN3).exists(), "odin3 (ODIN3_CLI) is needed")
class TimeLimitTest(unittest.TestCase):
    def test_limit_ends_the_run_with_a_warning_and_a_summary(self) -> None:
        done = run(f"sim_tb --top t --max-time 1000 {FOREVER}")
        self.assertEqual(0, done.returncode, done.stderr)
        self.assertIn("warning: sim_tb: stopped at the time limit --max-time 1000", done.stderr)
        self.assertIn("Raise it with --max-time", done.stderr)
        self.assertIn("sim_tb: stopped at time 1000 (time limit)", done.stderr)
        self.assertNotIn("finished at time", done.stderr)

    def test_default_limit_is_named(self) -> None:
        done = run(f"sim_tb --top t {FOREVER}")
        self.assertIn("--max-time 10000000 (default 10000000)", done.stderr)

    def test_finish_has_no_warning(self) -> None:
        done = run(f"sim_tb --top t {FINISH}")
        self.assertEqual(0, done.returncode, done.stderr)
        self.assertNotIn("time limit", done.stderr)
        self.assertIn("sim_tb: finished at time 10", done.stderr)


if __name__ == "__main__":
    unittest.main()
