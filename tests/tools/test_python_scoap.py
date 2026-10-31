"""Design.scoap through the Python binding (spec docs/specs/2026-10-31-scoap-design.md section 6).

Run through CTest `python_scoap`, which sets ODIN3_LIB. Skipped where cffi is missing, unless
ODIN3_REQUIRE_CFFI=1.
"""

from __future__ import annotations

import os
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
    from odin3 import Design

REPO_ROOT = Path(__file__).resolve().parents[2]
LIB = REPO_ROOT / "lib" / "vtr.o3lib"
AND2 = ".model t\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n"


@unittest.skipUnless(HAVE_CFFI or REQUIRE_CFFI, "cffi not installed")
@unittest.skipUnless(os.environ.get("ODIN3_LIB"), "ODIN3_LIB not set (run CTest python_scoap)")
class PythonScoapTest(unittest.TestCase):
    def test_design_scoap(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp, Design() as design:
            src = Path(tmp) / "t.blif"
            src.write_text(AND2)
            design.run_pass("read_techlib", str(LIB))
            design.run_pass("read_blif", str(src))
            result = design.scoap(top=3, sigprob=4000, threshold=0.3)
        self.assertEqual(result["schema"], "odin3-scoap")
        nets = {n["name"]: n for n in result["modules"][0]["nets"]}
        self.assertEqual((nets["y"]["cc0"], nets["y"]["cc1"], nets["a"]["co"]), (2, 3, 2))
        self.assertGreater(nets["y"]["p1_ppm"], 200_000)
        self.assertLess(nets["y"]["p1_ppm"], 300_000)
        rare = result["modules"][0]["rare"]
        self.assertEqual(
            [n["name"] for n in result["modules"][0]["nets"] if n["id"] in rare], ["y"]
        )

    def test_sequential_has_sc(self) -> None:
        import tempfile

        text = ".model t\n.inputs d clk\n.outputs q\n.latch d q re clk 0\n.end\n"
        with tempfile.TemporaryDirectory() as tmp, Design() as design:
            src = Path(tmp) / "t.blif"
            src.write_text(text)
            design.run_pass("read_techlib", str(LIB))
            design.run_pass("read_blif", str(src))
            result = design.scoap(sequential=True)
        nets = {n["name"]: n for n in result["modules"][0]["nets"]}
        self.assertEqual((nets["q"]["sc0"], nets["q"]["sc1"], nets["d"]["so"]), (1, 1, 1))


if __name__ == "__main__":
    unittest.main()
