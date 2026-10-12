"""Tests for the ABC build (3D spec AB-5): cmake/abc_pin.cmake and the `abc` tool.

The pin check runs cmake in script mode (ODIN3_CMAKE, else cmake on PATH). The tool test runs
$ODIN3_ABC, the build's odin3_abc_exe; it is skipped when ODIN3_ABC is unset, and fails instead
with ODIN3_REQUIRE_ABC=1 (set by the CTest run).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import helpers

PIN = "a3001b72edc5de22442e942165487fddf150e3d0"
ZERO = "0" * 40
ABC = os.environ.get("ODIN3_ABC", "")
REQUIRE_ABC = os.environ.get("ODIN3_REQUIRE_ABC") == "1"

# Two gates: n = a & b, y = n | c.
TWO_GATES = """.model t
.inputs a b c
.outputs y
.names a b n
11 1
.names n c y
1- 1
-1 1
.end
"""


def _pin_check(*defines: str) -> subprocess.CompletedProcess[str]:
    cmake = os.environ.get("ODIN3_CMAKE") or shutil.which("cmake")
    if cmake is None:
        raise unittest.SkipTest("no cmake")
    return subprocess.run(
        [cmake, "-DODIN3_ABC_SOURCE_DIR=third_party/abc", *defines, "-P",
         "cmake/abc_pin.cmake"],
        cwd=helpers.REPO_ROOT, capture_output=True, text=True, timeout=120, check=False)


class AbcPinTest(unittest.TestCase):
    def test_other_commit_stops(self) -> None:
        run = _pin_check(f"-DODIN3_ABC_HEAD_OVERRIDE={ZERO}")
        self.assertNotEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn(f"third_party/abc is at {ZERO}; ORACLES.md pins {PIN}", run.stderr)

    def test_pinned_commit_passes(self) -> None:
        run = _pin_check()
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(run.stderr, "")

    def test_pin_matches_oracles_md(self) -> None:
        oracles = (helpers.REPO_ROOT / "docs" / "ORACLES.md").read_text(encoding="utf-8")
        self.assertIn(f"`{PIN}`", oracles)


class AbcToolTest(unittest.TestCase):
    def test_strash_print_stats_no_history(self) -> None:
        if not ABC:
            if REQUIRE_ABC:
                self.fail("ODIN3_REQUIRE_ABC=1 but ODIN3_ABC is not set")
            self.skipTest("ODIN3_ABC not set")
        with tempfile.TemporaryDirectory() as work:
            (Path(work) / "t.blif").write_text(TWO_GATES, encoding="utf-8")
            run = subprocess.run([ABC, "-c", "read t.blif; strash; print_stats"], cwd=work,
                                 capture_output=True, text=True, timeout=120, check=False)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("and =", run.stdout)
            self.assertFalse((Path(work) / "abc.history").exists())  # ABC_NO_HISTORY
            self.assertEqual(sorted(p.name for p in Path(work).iterdir()), ["t.blif"])


if __name__ == "__main__":
    unittest.main()
