"""Hygiene gate: no tracked file may contain a user home path (the repo is public).

A path such as a user's home directory leaks the author's machine layout. Use `~/odin3-ws/...`
in docs and commands, or a neutral prefix inside tool-generated fixture text. Files that mention
the bare prefix on purpose (prose, or a test that asserts its absence) are allowlisted below, each
with its reason. third_party/ is vendored and excluded.
"""

from __future__ import annotations

import subprocess
import unittest

from . import helpers

# Built from parts so this file does not contain the pattern it searches for.
NEEDLE = "/home" + "/"

ALLOWED = {
    "tests/tools/test_no_home_paths.py": "this test",
    "docs/PHASE0.md": "prose naming the hard-coded form it replaced",
    "docs/specs/2026-10-11-3E-blif-vtr-design.md": "prose about --diag-paths resolved",
    "tests/tools/test_vtr_qor_exit.py": "asserts the pattern is absent from task configs",
    "tests/tools/test_vtr_qor_legs.py": "synthetic /home/u argument to golden_path",
}


class NoHomePathsTest(unittest.TestCase):
    @unittest.skipUnless((helpers.REPO_ROOT / ".git").exists(), "not a git checkout (tarball)")
    def test_tracked_files_have_no_home_path(self) -> None:
        listing = subprocess.run(
            ["git", "-C", str(helpers.REPO_ROOT), "ls-files", "-z"],
            check=True, capture_output=True,
        ).stdout.decode("utf-8").split("\0")
        bad = []
        for name in listing:
            if not name or name.startswith("third_party/") or name in ALLOWED:
                continue
            path = helpers.REPO_ROOT / name
            if not path.is_file() or path.is_symlink():
                continue
            if NEEDLE.encode() in path.read_bytes():
                bad.append(name)
        self.assertEqual(bad, [], f"tracked files contain {NEEDLE!r}; use ~/ or a neutral prefix")


if __name__ == "__main__":
    unittest.main()
