"""Tests for tools/icells-check/icells_signature.py (5.2 review M2): the per-cell signature is
stable under renaming of generated nets and reordering of cells, and changes with a cell type, a
parameter, a port width or the wiring."""

from __future__ import annotations

import unittest

import icells_signature as sg

BASE = """module \\top
  wire width 4 input 1 \\a
  wire width 4 input 2 \\b
  wire width 4 output 3 \\y
  wire width 4 $n7
  cell $add $c1
    parameter \\A_WIDTH 4
    parameter \\Y_WIDTH 4
    connect \\A \\a
    connect \\B \\b
    connect \\Y $n7
  end
  cell $not $c2
    parameter \\A_WIDTH 4
    connect \\A $n7
    connect \\Y \\y
  end
end
"""


def sig(text: str) -> dict[str, object]:
    return dict(sg.signature(text))


class SignatureTests(unittest.TestCase):
    def test_counts(self) -> None:
        self.assertEqual(sg.cell_count(sg.signature(BASE)), 2)
        self.assertEqual(sg.cell_count(sg.signature("")), 0)

    def test_renaming_and_order_do_not_matter(self) -> None:
        renamed = BASE.replace("$n7", "\\$n99").replace("$c1", "$c5").replace("$c2", "$c4")
        swapped = BASE.replace("$c1", "$cx").replace("$c2", "$c1").replace("$cx", "$c2")
        self.assertEqual(sig(BASE), sig(renamed))
        self.assertEqual(sig(BASE), sig(swapped))

    def test_an_alias_wire_adds_nothing(self) -> None:
        aliased = BASE.replace("  wire width 4 $n7\n", "  wire width 4 $n7\n  wire width 4 $a8\n")
        aliased = aliased.rstrip("\n")[: -len("end")] + "  connect $a8 $n7\nend\n"
        self.assertEqual(sig(BASE), sig(aliased))

    def test_a_parameter_change_is_found(self) -> None:
        self.assertNotEqual(sig(BASE), sig(BASE.replace("\\Y_WIDTH 4", "\\Y_WIDTH 5")))

    def test_a_type_change_is_found(self) -> None:
        self.assertNotEqual(sig(BASE), sig(BASE.replace("cell $add", "cell $sub")))

    def test_a_width_change_is_found(self) -> None:
        wider = BASE.replace("connect \\B \\b", "connect \\B { 1'0 \\b [2:0] }")
        self.assertNotEqual(sig(BASE), sig(wider))

    def test_a_rewiring_is_found(self) -> None:
        self.assertNotEqual(sig(BASE), sig(BASE.replace("connect \\B \\b", "connect \\B \\a")))

    def test_a_constant_is_found(self) -> None:
        one = BASE.replace("connect \\B \\b", "connect \\B 4'0001")
        two = BASE.replace("connect \\B \\b", "connect \\B 4'0010")
        self.assertNotEqual(sig(one), sig(two))

    def test_swapped_inputs_are_found(self) -> None:
        swapped = (
            BASE.replace("connect \\A \\a", "connect \\A \\X")
            .replace("connect \\B \\b", "connect \\B \\a")
            .replace("connect \\A \\X", "connect \\A \\b")
        )
        self.assertNotEqual(sig(BASE), sig(swapped))


if __name__ == "__main__":
    unittest.main()
