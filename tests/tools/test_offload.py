"""Tests for tools/vtr-qor/offload/run-offload (PHASE3 decision 54), through its --dry-run.

Nothing runs: --dry-run prints each command. The work directory is a private mkdtemp."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import qor_sched

from . import helpers

SCRIPT = helpers.REPO_ROOT / "tools" / "vtr-qor" / "offload" / "run-offload"
OFFLOAD_DESIGNS = ("LU8PEEng", "bgm", "mcml", "LU32PEEng")


class RunOffloadTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="offload-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def run_script(self, *args: str) -> subprocess.CompletedProcess[str]:
        env = {**os.environ, "OFFLOAD_OUT": str(self.tmp / "out"),
               "ODIN3_QOR_DIR": str(self.tmp / "qor"), "YOSYS": "/nonexistent"}
        return subprocess.run(["bash", str(SCRIPT), *args], env=env, text=True,
                              capture_output=True, check=False)

    def test_vtr_commands(self) -> None:
        done = self.run_script("--dry-run", "vtr")
        self.assertEqual(done.returncode, 0, done.stderr)
        lines = [ln for ln in done.stdout.splitlines() if ln.startswith("+ ")]
        self.assertEqual(len(lines), 4)
        self.assertIn("--designs LU8PEEng,bgm --fcheck --keep-netlists", lines[0])
        self.assertIn("--designs mcml,LU32PEEng --xl --fcheck --keep-netlists", lines[1])
        self.assertIn("fcheck --leg P --set offload --resume", lines[2])
        self.assertIn("table --legs P,Oprime,Pprime,O --set offload", lines[3])
        for line in lines[:2]:
            self.assertIn("run --leg P,Oprime,Pprime,O", line)
        self.assertNotIn("--set gate", done.stdout)

    def test_no_xl(self) -> None:
        done = self.run_script("--dry-run", "--no-xl", "vtr")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertNotIn("--xl", done.stdout)

    def test_long_runs(self) -> None:
        done = self.run_script("--dry-run", "lower", "fuzz")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("--seeds 1..5000 --min-compared 4500 --tier-t report", done.stdout)
        self.assertIn("-max_total_time=3600", done.stdout)

    def test_abc_steps(self) -> None:
        done = self.run_script("--dry-run", "abc")
        self.assertEqual(done.returncode, 0, done.stderr)
        lines = [ln for ln in done.stdout.splitlines() if ln.startswith("+ env")]
        self.assertEqual(len(lines), 16)  # H1 (2), H2, H3, H4 (2), H5, H6 for two designs
        text = done.stdout
        self.assertLess(text.index("mcml/mcml.parmys.blif"),
                        text.index("LU32PEEng/LU32PEEng.parmys.blif"))
        for design in ("LU8PEEng", "bgm"):
            self.assertNotIn(f"{design}.parmys.blif", text)

    def test_usage(self) -> None:
        self.assertEqual(self.run_script().returncode, 2)
        done = self.run_script("--dry-run", "nope")
        self.assertEqual(done.returncode, 2)
        self.assertIn("unknown argument", done.stderr)

    def test_sets_json_offload_matches_script(self) -> None:
        text = SCRIPT.read_text(encoding="utf-8")
        for design in qor_sched.load_sets().offload:
            self.assertIn(design, text)
        self.assertEqual(set(qor_sched.load_sets().offload), set(OFFLOAD_DESIGNS))


if __name__ == "__main__":
    unittest.main()
