"""Graph construction of the graphlets pass (spec docs/specs/2026-10-30-graphlets-design.md,
as-built note on the `$sop`/`$lut` fix): the pass's counted graph equals an independent E-4 build
from the written JSON (tools/graphlets/construction_gate.py).

Cases: a 2-input `.names` AND is 4 nodes (a, b, y and the cell) and 3 edges; a constant driver is
left out; the same AND mapped to a `$lut` and to gate cells; the committed golden BLIFs in LUT and
gate form. Run through CTest `graphlets_construction` (ODIN3_BIN set); skipped without it.
"""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import ModuleType

REPO_ROOT = Path(__file__).resolve().parents[2]
ODIN3 = os.environ.get("ODIN3_BIN")
LIB = REPO_ROOT / "lib" / "vtr.o3lib"
GATES = REPO_ROOT / "lib" / "cmos6.o3lib"

AND2 = ".model t\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n"
# c is the constant 1, y = a AND c
CONST = ".model t\n.inputs a\n.outputs y\n.names c\n1\n.names a c y\n11 1\n.end\n"


def load_gate() -> ModuleType:
    path = REPO_ROOT / "tools" / "graphlets" / "construction_gate.py"
    spec = importlib.util.spec_from_file_location("construction_gate", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest graphlets_construction)")
class GraphletsConstructionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.gate = load_gate()
        self.graph = self.gate.load_graph_module()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def counts(self, text: str, extra: str = "") -> tuple[int, int]:
        """(nodes, edges) the pass reports for a BLIF text, after the optional extra pass."""
        blif = self.dir / "t.blif"
        blif.write_text(text)
        out = self.dir / "g.json"
        libs = f"read_techlib {LIB}; read_techlib {GATES}"
        steps = f"{libs}; read_blif {blif}; {extra}graphlets --out {out}"
        done = subprocess.run([str(ODIN3), "-p", steps], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        group = json.loads(out.read_text())["groups"][0]
        return group["nodes"], group["edges"]

    def agree(self, text: str, form: str) -> None:
        blif = self.dir / "t.blif"
        blif.write_text(text)
        status, mine, ref, note = self.gate.check_one(
            self.graph, str(ODIN3), str(LIB), blif, form, GATES, self.dir
        )
        self.assertEqual(status, "ok", f"{note} pass={mine} reference={ref}")

    def test_and2_names_is_four_nodes_three_edges(self) -> None:
        self.assertEqual(self.counts(AND2), (4, 3))

    def test_constant_driver_is_left_out(self) -> None:
        self.assertEqual(self.counts(CONST), (3, 2))  # ports a, y and the AND; not the constant

    def test_lut_mapped_and2(self) -> None:
        self.assertEqual(self.counts(AND2, "abc --lut 4; "), (4, 3))

    def test_gate_mapped_and2(self) -> None:
        nodes, edges = self.counts(AND2, "abc --gates cmos6; ")
        self.assertGreaterEqual(nodes, 4)
        self.assertGreaterEqual(edges, 3)

    def test_pass_equals_e4_build_synthetic(self) -> None:
        for text in (AND2, CONST):
            for form in ("lut", "gates"):
                with self.subTest(form=form):
                    self.agree(text, form)

    def test_pass_equals_e4_build_golden_blifs(self) -> None:
        blifs = sorted((REPO_ROOT / "tests" / "golden" / "blif").glob("*.parmys.blif"))
        self.assertTrue(blifs)
        seen = 0
        for blif in blifs:
            for form in ("lut", "gates"):
                status, mine, ref, note = self.gate.check_one(
                    self.graph, str(ODIN3), str(LIB), blif, form, GATES, self.dir
                )
                self.assertNotEqual(status, "DIFFER", f"{blif.name} {form}: {note} {mine} {ref}")
                seen += status == "ok"
        self.assertGreater(seen, 0)


if __name__ == "__main__":
    unittest.main()
