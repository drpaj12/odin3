"""Tests for tools/golden-blackboxes."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import golden_blackboxes

from . import helpers

ADDER_PARMYS = """.model top
.inputs x
.end

.model adder
.inputs a \\
  b cin   # a comment
.outputs cout sumout
.blackbox
.end
"""

ADDER_ODIN = """.model top
.end
.model adder
.inputs a[0] b[0] cin[0]
.outputs cout[0] sumout[0]
.blackbox
.end
"""


class GoldenBlackboxesTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.golden = self.root / "golden"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _put(self, rel: str, text: str, status: str = "ok") -> Path:
        path = self.golden / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        path.with_suffix(".prov").write_text(f"tool=x\nstatus={status}\n")
        return path

    def test_distinct_stanzas_counted_in_first_seen_order(self) -> None:
        self._put("A/d1/d1.parmys.blif", ADDER_PARMYS)
        self._put("A/d2/d2.parmys.blif", ADDER_PARMYS + "\n")
        self._put("A/d3/d3.odin.blif", ADDER_ODIN)
        self._put("A/d4/d4.odin.blif", ADDER_ODIN, status="failed")
        stanzas = golden_blackboxes.collect(golden_blackboxes.golden_blifs(self.golden))
        self.assertEqual([(s.oracle, s.count) for s in stanzas], [("odin", 1), ("parmys", 2)])
        self.assertEqual(stanzas[1].lines, [".inputs a b cin", ".outputs cout sumout"])
        self.assertEqual(stanzas[0].inputs, ["a[0]", "b[0]", "cin[0]"])

    def test_write_makes_one_fixture_per_stanza(self) -> None:
        self._put("A/d1/d1.parmys.blif", ADDER_PARMYS)
        out = self.root / "out"
        out.mkdir()
        (out / "stale.blif").write_text("old")
        code, stdout, _ = helpers.run_main(
            golden_blackboxes.main, ["--golden", str(self.golden), "--out", str(out), "--write"]
        )
        self.assertEqual(code, 0)
        self.assertIn("1 distinct black-box stanzas", stdout)
        self.assertEqual(sorted(p.name for p in out.iterdir()), ["adder.parmys.01.blif"])
        text = (out / "adder.parmys.01.blif").read_text()
        self.assertIn("# 'adder' as A/d1/d1.parmys.blif declares it (1 golden files", text)
        self.assertIn(".subckt adder a=i_a b=i_b cin=i_cin cout=o_cout sumout=o_sumout\n", text)
        self.assertTrue(text.endswith(".model adder\n.inputs a b cin\n.outputs cout sumout\n"
                                      ".blackbox\n.end\n"))

    def test_missing_golden_is_an_input_error(self) -> None:
        code, _, stderr = helpers.run_main(golden_blackboxes.main, ["--golden", str(self.golden)])
        self.assertEqual(code, 2)
        self.assertIn("not a directory", stderr)


if __name__ == "__main__":
    unittest.main()
