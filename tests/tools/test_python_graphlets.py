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

    def test_default_max_work_is_bounded(self) -> None:
        import inspect

        default = inspect.signature(odin3.graphlet_counts).parameters["max_work"].default
        self.assertEqual(default, 20_000_000_000)
        method = inspect.signature(odin3.Odin3.graphlet_counts).parameters["max_work"].default
        self.assertEqual(method, 20_000_000_000)

    def test_missing_symbol_is_a_clear_error(self) -> None:
        class OldLib:
            pass

        old = odin3.Odin3.__new__(odin3.Odin3)
        old.lib = OldLib()
        with self.assertRaises(odin3.Odin3Error) as ctx:
            old.graphlet_counts(2, [(0, 1)])
        self.assertIn("libodin3 too old", str(ctx.exception))
        self.assertIn("odin3_graphlets_count", str(ctx.exception))

    def test_instances_hand_cases(self) -> None:
        total, found = odin3.graphlet_instances(5, [(0, 1), (1, 2), (2, 3), (3, 4)], graphlet=1)
        self.assertEqual(total, 3)
        self.assertEqual(sorted(found), [(0, 1, 2), (1, 2, 3), (2, 3, 4)])
        # a triangle with a pendant node: one triangle (G2), two 3-node paths (G1)
        edges = [(0, 1), (1, 2), (2, 0), (2, 3)]
        self.assertEqual(odin3.graphlet_instances(4, edges, graphlet=2), (1, [(0, 1, 2)]))
        total, found = odin3.graphlet_instances(4, edges, graphlet=1)
        self.assertEqual((total, sorted(found)), (2, [(0, 2, 3), (1, 2, 3)]))

    def test_instances_max_truncates_total_exact(self) -> None:
        total, found = odin3.graphlet_instances(5, [(0, 1), (1, 2), (2, 3), (3, 4)], 1, max=1)
        self.assertEqual((total, len(found)), (3, 1))
        self.assertEqual(odin3.graphlet_instances(2, [(0, 1)], 0, max=0), (1, []))

    def test_instances_total_matches_orbit_totals(self) -> None:
        rng = random.Random(11)
        for _ in range(6):
            n = rng.randint(5, 9)
            pairs = [(a, b) for a in range(n) for b in range(a + 1, n)]
            edges = [e for e in pairs if rng.random() < 0.4]
            rows = odin3.graphlet_counts(n, edges, size=5)
            # orbit 0 is the degree: sum / 2 = edges = instances of G0
            self.assertEqual(odin3.graphlet_instances(n, edges, 0)[0], sum(r[0] for r in rows) // 2)

    def test_instances_errors(self) -> None:
        with self.assertRaises(odin3.Odin3Error):
            odin3.graphlet_instances(2, [(0, 1)], graphlet=30)
        with self.assertRaises(ValueError):
            odin3.graphlet_instances(2, [(0, 1)], graphlet=0, size=3)

    def test_similarity_full_precision(self) -> None:
        rows = odin3.graphlet_counts(5, [(0, 1), (1, 2), (2, 3), (3, 4), (1, 3)], size=5)
        same = odin3.graphlet_similarity(rows, rows, 73)
        self.assertEqual(same, (1.0, 1.0))
        gdd, cosine = odin3.graphlet_similarity(rows[:2], rows[2:], 73)
        self.assertTrue(0.0 <= gdd < 1.0 and 0.0 <= cosine < 1.0)
        # not a 6-decimal print: the value is the double the C metric computed
        self.assertNotEqual(gdd, round(gdd, 6))
        self.assertEqual(odin3.graphlet_similarity([], [], 73), (1.0, 1.0))
        with self.assertRaises(odin3.Odin3Error):
            odin3.graphlet_similarity([[1, 2, 3]], [[1, 2, 3]], 3)
        with self.assertRaises(ValueError):
            odin3.graphlet_similarity(rows, rows, 15)

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
