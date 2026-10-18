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

    def test_a_pmux_fixture_is_a_parallel_case_with_one_item_per_select(self) -> None:
        cell = gc.pmux_cell(2, 3)
        self.assertEqual(cell.stem, "pmux_w2s3")
        self.assertIn("(* parallel_case *) case (1'b1)", cell.text)
        self.assertEqual(cell.text.count("y = b"), 3)
        self.assertIn("input [2:0] s", cell.text)

    def test_the_arith_family_has_add_sub_and_neg_and_no_duplicate_stems(self) -> None:
        stems = [c.stem for c in gc.generate(["arith"])]
        self.assertEqual(len(stems), len(set(stems)))
        for prefix in ("add_", "sub_", "neg_"):
            self.assertTrue(any(s.startswith(prefix) for s in stems), prefix)

    def test_the_mul_and_div_families_cover_the_operators_and_their_shapes(self) -> None:
        by_stem = {c.stem: c for c in gc.generate(["mul", "div"])}
        self.assertEqual(len(by_stem), len(gc.generate(["mul", "div"])))
        self.assertIn("assign y = a * b;", by_stem["mul_w3x5"].text)
        self.assertIn("assign y = a / b;", by_stem["div_w4_s"].text)
        self.assertIn("assign y = a % b;", by_stem["mod_w5x3_ms"].text)
        self.assertIn("module m(input signed [3:0] a, input signed [3:0] b, output [2:0] y);",
                      by_stem["mul_w4_s_y3"].text)
        for prefix in ("mul_", "div_", "mod_"):
            self.assertTrue(any(s.startswith(prefix) for s in by_stem), prefix)

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

    def test_the_mem_family_has_scalar_one_bit_ports_and_a_falling_clock_variant(self) -> None:
        by_stem = {c.stem: c for c in gc.generate(["mem"])}
        self.assertEqual(12, len(by_stem))
        self.assertIn("input wa, input wd, input ra, output rd);", by_stem["mem_w1_d2"].text)
        self.assertIn("negedge clk", by_stem["mem_w4_d4_neg"].text)
        self.assertNotIn("posedge", by_stem["mem_w4_d4_neg"].text)
        self.assertIn("reg [2:0] mem [-2:5];", by_stem["mem_w3_d8_offm2"].text)
        # SIZE 8 above 2^ABITS 4 at OFFSET 1 (3B-3 L-1)
        self.assertIn("reg [3:0] mem [1:8];", by_stem["mem_w4_d8_a2_off1"].text)
        self.assertIn("input [1:0] ra", by_stem["mem_w4_d8_a2_off1"].text)

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

    def test_the_ff_family_has_every_type_at_three_widths_and_the_polarity_variants(self) -> None:
        cells = {c.stem: c for c in gc.generate(["ff"])}
        self.assertEqual(len(cells), 64)
        for kind in gc.FF_KINDS:
            for width in (1, 4, 8):
                self.assertIn(f"{kind}_w{width}", cells)
            self.assertIn(f"{kind}_w4_neg", cells)
            self.assertEqual(f"{kind}_w4_lo" in cells, kind != "dff")
        text = cells["adffe_w4_neg_lo"].text
        self.assertIn("always @(negedge clk or negedge rst) if (!rst) q <= 4'd5; "
                      "else if (!en) q <= d;", text)
        self.assertIn("input en", cells["sdffce_w1"].text)
        self.assertNotIn("input en", cells["sdff_w1"].text)
        self.assertIn("4'b1010", cells["dffsr_w4"].text)
        self.assertIn("input [3:0] ad", cells["aldffe_w4"].text)

    def test_an_unknown_family_is_refused(self) -> None:
        with self.assertRaises(SystemExit):
            gc.generate(["nosuch"])


if __name__ == "__main__":
    unittest.main()
