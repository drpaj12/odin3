"""Graphlets through the Python binding (spec docs/specs/2026-10-30-graphlets-design.md section 8
and 17): the graph-generic counts for a plain edge list against an independent plain-Python
reference (three-way with the C brute force, see tools/graphlets/directed_checks.py),
Design.graphlets and graphlet_compare.

Run through CTest `python_graphlets`, which sets ODIN3_LIB and ODIN3_GLET_COUNT. Skipped where cffi
is missing, unless ODIN3_REQUIRE_CFFI=1.
"""

from __future__ import annotations

import importlib.util
import os
import random
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
    import odin3
    from odin3 import Design

REPO_ROOT = Path(__file__).resolve().parents[2]
BLIF = REPO_ROOT / "tests" / "golden" / "blif" / "ff.parmys.blif"
LIB = REPO_ROOT / "lib" / "vtr.o3lib"


def load_checks() -> object:
    spec = importlib.util.spec_from_file_location(
        "directed_checks", REPO_ROOT / "tools" / "graphlets" / "directed_checks.py"
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(HAVE_CFFI or REQUIRE_CFFI, "cffi not installed")
@unittest.skipUnless(os.environ.get("ODIN3_LIB"), "ODIN3_LIB not set (run CTest python_graphlets)")
class PythonGraphletsTest(unittest.TestCase):
    def test_path_counts(self) -> None:
        rows = odin3.graphlet_counts(4, [(0, 1), (1, 2), (2, 3)], size=4)
        self.assertEqual([row[0] for row in rows], [1, 2, 2, 1])  # orbit 0 is the degree

    def test_bad_size(self) -> None:
        with self.assertRaises(ValueError):
            odin3.graphlet_counts(2, [(0, 1)], size=3)

    @unittest.skipUnless(os.environ.get("ODIN3_GLET_COUNT"), "ODIN3_GLET_COUNT not set")
    def test_directed_against_python_reference(self) -> None:
        checks = load_checks()
        cat = checks.Catalogue(os.environ["ODIN3_GLET_COUNT"], True)  # type: ignore[attr-defined]
        rng = random.Random(3)
        for i in range(40):
            n, edges = checks.random_digraph(rng, i % 2 == 0)  # type: ignore[attr-defined]
            ref = checks.reference(n, edges, cat)  # type: ignore[attr-defined]
            got = odin3.graphlet_counts(n, sorted(edges), directed=True)
            self.assertEqual(ref, got)

    def test_design_graphlets_and_compare(self) -> None:
        with Design() as design:
            design.run_pass("read_techlib", str(LIB))
            design.run_pass("read_blif", str(BLIF))
            result = design.graphlets(per_node=True)
            self.assertEqual(result["schema"], "odin3-graphlets")
            group = result["groups"][0]
            self.assertEqual(len(group["orbit_totals"]), 73)
            self.assertEqual(len(group["node_vectors"]), group["reported"])
            top = design.top()
            assert top is not None
            totals = top.attr("odin3:glet_orbit_totals")
            assert totals is not None
            self.assertEqual([int(x) for x in totals.split(",")], group["orbit_totals"])
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / "sig.json"
                design.run_pass("graphlets", f'--out "{path}"')
                same = odin3.graphlet_compare(path, path)
                self.assertAlmostEqual(same["gdd_agreement"], 1.0)
                self.assertAlmostEqual(same["cosine_log"], 1.0)
                self.assertFalse(same.get("calibrated", False))


if __name__ == "__main__":
    unittest.main()
