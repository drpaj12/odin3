"""Tests for tests/darkriscv/uart_text.py: UART lines must agree and cover the expected text."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "darkriscv" / "uart_text.py"
SPEC = importlib.util.spec_from_file_location("uart_text", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
uart_text = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(uart_text)

LINES = ["UART 10 41 A", "UART 20 0d .", "UART 30 0a .", "UART 40 42 B"]


class Check(unittest.TestCase):
    def test_agree_and_cover(self) -> None:
        self.assertIsNone(uart_text.check(LINES, LINES, "A\nB", True))

    def test_text_is_a_prefix_of_expect(self) -> None:
        self.assertIsNone(uart_text.check(LINES[:3], LINES[:3], "A\nBC", False))
        self.assertIsNotNone(uart_text.check(LINES[:3], LINES[:3], "A\nBC", True))

    def test_trailing_spaces_are_ignored(self) -> None:
        lines = ["UART 10 41 A", "UART 20 20  ", "UART 30 0a .", "UART 40 42 B"]
        self.assertIsNone(uart_text.check(lines, lines, "A\nB", True))

    def test_stuck_bit_changes_a_byte(self) -> None:
        out = [*LINES[:3], "UART 40 43 C"]
        self.assertIsNotNone(uart_text.check(LINES, out, "A\nB", False))

    def test_timing_difference_fails(self) -> None:
        out = ["UART 11 41 A", *LINES[1:]]
        self.assertIsNotNone(uart_text.check(LINES, out, "A\nB", False))

    def test_wrong_text_and_empty_fail(self) -> None:
        self.assertIsNotNone(uart_text.check(LINES, LINES, "X", False))
        self.assertIsNotNone(uart_text.check([], [], "A", False))


if __name__ == "__main__":
    unittest.main()
