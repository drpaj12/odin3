"""Tests for tools/opt-check/il_order.py: the operand-first order of an RTLIL file's cells."""

from __future__ import annotations

import unittest

import il_order


def cell(kind: str, name: str, **ports: str) -> str:
    lines = [f"  cell {kind} {name}"]
    lines += [f"    connect \\{port} {sig}" for port, sig in ports.items()]
    return "\n".join([*lines, "  end"])


def module(*parts: str) -> str:
    return "\n".join(["module \\m", "  wire $n1", *parts, "end"]) + "\n"


def names(text: str) -> list[str]:
    return [line.split()[2] for line in text.splitlines() if line.startswith("  cell ")]


class TestOrder(unittest.TestCase):
    def test_operand_goes_before_its_consumer(self) -> None:
        text = module(cell("$add", "$c1", A="$n1", B="$n2", Y="$n3"),
                      cell("$sub", "$c2", A="\\m", B="$n9", Y="$n1"))
        self.assertEqual(names(il_order.reorder_text(text)), ["$c2", "$c1"])

    def test_ties_keep_file_order(self) -> None:
        text = module(cell("$and", "$c1", A="\\a", Y="$n1"), cell("$or", "$c2", A="\\b", Y="$n2"))
        self.assertEqual(names(il_order.reorder_text(text)), ["$c1", "$c2"])

    def test_port_order_decides_between_two_operands(self) -> None:
        text = module(cell("$add", "$c1", A="$n1", B="$n2", Y="$n3"),
                      cell("$not", "$c2", A="\\x", Y="$n2"),
                      cell("$not", "$c3", A="\\y", Y="$n1"))
        self.assertEqual(names(il_order.reorder_text(text)), ["$c3", "$c2", "$c1"])

    def test_flip_flop_output_is_not_followed(self) -> None:
        text = module(cell("$not", "$c1", A="$n5", Y="$n6"),
                      cell("$dff", "$c2", D="$n6", Q="$n5"))
        self.assertEqual(names(il_order.reorder_text(text)), ["$c1", "$c2"])

    def test_loop_terminates(self) -> None:
        text = module(cell("$not", "$c1", A="$n2", Y="$n1"), cell("$not", "$c2", A="$n1", Y="$n2"))
        self.assertEqual(sorted(names(il_order.reorder_text(text))), ["$c1", "$c2"])

    def test_module_connect_is_followed(self) -> None:
        text = module("  connect \\t { $n4 $n5 }",
                      cell("$not", "$c1", A="\\t", Y="$n6"),
                      cell("$not", "$c2", A="\\z", Y="$n5"))
        self.assertEqual(names(il_order.reorder_text(text)), ["$c2", "$c1"])

    def test_attributes_move_with_their_cell(self) -> None:
        text = module(cell("$add", "$c1", A="$n1", Y="$n3"),
                      "  attribute \\keep 1\n" + cell("$not", "$c2", A="\\x", Y="$n1"))
        lines = il_order.reorder_text(text).splitlines()
        self.assertEqual(lines[lines.index("  attribute \\keep 1") + 1], "  cell $not $c2")

    def test_non_cell_lines_stay(self) -> None:
        text = module(cell("$add", "$c1", A="$n1", Y="$n3"), cell("$not", "$c2", A="\\x", Y="$n1"))
        out = il_order.reorder_text(text)
        self.assertEqual(out.splitlines()[:2], ["module \\m", "  wire $n1"])
        self.assertEqual(out.splitlines()[-1], "end")


if __name__ == "__main__":
    unittest.main()
