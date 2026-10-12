"""Tests for tools/check-symbols.sh's --only/--forbid mode (3D spec §4.2, the ABC adapter)."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import helpers

SCRIPT = helpers.REPO_ROOT / "tools" / "check-symbols.sh"
ONLY = "odin3_plugin_init,odin3_abc_adapter_init"
FORBID = "exit,_exit,abort"

GOOD = """
int odin3_plugin_init(unsigned v) { return (int)v; }
int odin3_abc_adapter_init(void) { return 0; }
"""
BAD = """
#include <stdlib.h>
int odin3_plugin_init(unsigned v) { return (int)v; }
int extra_fn(void) { return 1; }
void quit_fn(void) { exit(1); }
"""


@unittest.skipUnless(shutil.which("cc") and shutil.which("nm"), "needs cc and nm")
class CheckSymbolsOnlyTest(unittest.TestCase):
    tmp: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = Path(tempfile.mkdtemp(prefix="check-symbols-"))
        for name, source in (("good", GOOD), ("bad", BAD)):
            (cls.tmp / f"{name}.c").write_text(source, encoding="utf-8")
            subprocess.run(
                ["cc", "-shared", "-fPIC", "-o", str(cls.tmp / f"{name}.so"),
                 str(cls.tmp / f"{name}.c")],
                check=True,
            )

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp)

    def run_script(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(SCRIPT), *args], capture_output=True, text=True, check=False
        )

    def test_exact_exports_and_no_forbidden_import_pass(self) -> None:
        lib = str(self.tmp / "good.so")
        result = self.run_script("--only", ONLY, "--forbid", FORBID, lib)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")

    def test_every_violation_has_its_line(self) -> None:
        lib = str(self.tmp / "bad.so")
        result = self.run_script("--only", ONLY, "--forbid", FORBID, lib)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(
            result.stderr.splitlines(),
            [
                f"check-symbols: {lib} exports extra_fn, which is not in --only",
                f"check-symbols: {lib} exports quit_fn, which is not in --only",
                f"check-symbols: {lib} does not export odin3_abc_adapter_init",
                f"check-symbols: {lib} imports forbidden exit",
            ],
        )

    def test_usage_errors_exit_2(self) -> None:
        lib = str(self.tmp / "good.so")
        for args in (
            ("--only", ONLY),
            ("--only", ONLY, "--forbid", lib),
            ("--only", ONLY, "--nope", FORBID, lib),
            ("--only", ",", "--forbid", FORBID, lib),
            ("--only", ONLY, "--forbid", FORBID, lib, lib),
        ):
            with self.subTest(args=args):
                result = self.run_script(*args)
                self.assertEqual(result.returncode, 2)
                self.assertIn("usage: tools/check-symbols.sh --only", result.stderr)

    def test_unreadable_library_exits_2(self) -> None:
        missing = str(self.tmp / "missing.so")
        result = self.run_script("--only", ONLY, "--forbid", FORBID, missing)
        self.assertEqual(result.returncode, 2)
        self.assertIn(f"check-symbols: cannot read the dynamic symbols of {missing}",
                      result.stderr)


if __name__ == "__main__":
    unittest.main()
