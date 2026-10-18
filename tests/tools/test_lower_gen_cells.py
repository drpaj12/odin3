"""Tests for tools/lower-check/gen_cells.py (3B Task 2 step 4): the per-cell fixture grid."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import gen_cells as gc

from . import helpers


class GridTest(unittest.TestCase):
    def test_stems_are_unique_and_the_grid_is_big_enough(self) -> None:
        cells = gc.generate(["bitwise"])
        stems = [c.stem for c in cells]
        self.assertEqual(len(stems), len(set(stems)))
        self.assertGreater(len(cells), 100)

    def test_module_text_of_a_mixed_sign_binary_cell(self) -> None:
        by_stem = {c.stem: c for c in gc.generate(["bitwise"])}
        self.assertEqual(
            by_stem["and_w3x5_ms"].text,
            "module m(input signed [2:0] a, input [4:0] b, output [4:0] y);\n"
            "  assign y = a & b;\nendmodule\n")
        self.assertIn("assign y = s ? b : a;", by_stem["mux_w8"].text)
        self.assertIn("assign y = a == b;", by_stem["eq_w5"].text)
        self.assertIn("assign y = ~^a;", by_stem["reduce_xnor_w3"].text)
        self.assertIn("module m(input a, output y);", by_stem["logic_not_w1"].text)

    def test_a_one_bit_port_has_no_range(self) -> None:
        by_stem = {c.stem: c for c in gc.generate(["bitwise"])}
        self.assertIn("module m(input a, input b, output y);", by_stem["and_w1"].text)

    def test_writing_twice_gives_identical_files(self) -> None:
        out = Path(tempfile.mkdtemp(prefix="gen_cells_test_"))
        for _ in range(2):
            code, _, _ = helpers.run_main(gc.main, ["--out", str(out)])
            self.assertEqual(code, 0)
        self.assertEqual((out / "and_w5.v").read_text(),
                         {c.stem: c for c in gc.generate(["bitwise"])}["and_w5"].text)
        code, text, _ = helpers.run_main(gc.main, ["--list"])
        self.assertEqual(code, 0)
        self.assertEqual(sorted(p.name for p in out.glob("*.v")), sorted(text.split()))

    def test_an_unknown_family_is_refused(self) -> None:
        with self.assertRaises(SystemExit):
            gc.generate(["nosuch"])


if __name__ == "__main__":
    unittest.main()
