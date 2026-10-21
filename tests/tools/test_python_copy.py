"""Phase 7, Task 6 (spec 3.2): Design.copy() over odin3_design_copy.

Run through CTest `python_copy`, which sets ODIN3_LIB (and preloads the sanitizer runtime for a
Debug library). Skipped where cffi is missing, unless ODIN3_REQUIRE_CFFI=1, and where ODIN3_LIB is
unset.
"""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

try:
    import cffi  # noqa: F401

    HAVE_CFFI = True
except ImportError:
    HAVE_CFFI = False
REQUIRE_CFFI = os.environ.get("ODIN3_REQUIRE_CFFI") == "1"

if TYPE_CHECKING or HAVE_CFFI:
    from odin3 import Design, Odin3

REPO_ROOT = Path(__file__).resolve().parents[2]
VERILOG = REPO_ROOT / "tests" / "micro" / "verilog" / "full" / "ansiportlist_2.v"


def json_of(design: Design, scratch: Path) -> str:
    """The text of write_json --odin3 (it carries the provenance too)."""
    out = scratch / "design.json"
    design.run_pass("write_json", f'"{out}" --odin3')
    return out.read_text(encoding="utf-8")


@unittest.skipUnless(HAVE_CFFI or REQUIRE_CFFI, "cffi not installed")
@unittest.skipUnless(os.environ.get("ODIN3_LIB"), "ODIN3_LIB not set (run CTest python_copy)")
class PythonCopyTest(unittest.TestCase):
    odin3: Odin3

    @classmethod
    def setUpClass(cls) -> None:
        if not HAVE_CFFI:
            raise AssertionError("ODIN3_REQUIRE_CFFI=1 but cffi is not installed")
        cls.odin3 = Odin3()
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_ERROR)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_INFO)

    def loaded(self) -> Design:
        design = Design(self.odin3)
        design.run_pass("read_verilog", f'"{VERILOG}"')
        design.run_pass("proc")
        return design

    def test_copy_independent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, self.loaded() as src:
            scratch = Path(tmp)
            with src.copy() as dup:
                self.assertEqual(json_of(dup, scratch), json_of(src, scratch))
                before = json_of(src, scratch)
                dup.run_pass("opt")
                self.assertEqual(json_of(src, scratch), before)
                self.assertNotEqual(json_of(dup, scratch), before)

    def test_copy_outlives_source(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            scratch = Path(tmp)
            src = self.loaded()
            want = json_of(src, scratch)
            dup = src.copy()
            src.close()
            self.assertEqual(json_of(dup, scratch), want)
            dup.close()

    def test_copy_keeps_module_ids(self) -> None:
        with self.loaded() as src, src.copy() as dup:
            self.assertEqual(
                [module.name for module in src.modules()],
                [module.name for module in dup.modules()],
            )


if __name__ == "__main__":
    unittest.main()
