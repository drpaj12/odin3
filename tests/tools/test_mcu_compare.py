"""Tests for tests/mcu/compare.py: the x-mask applies to the reset window only."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "mcu" / "compare.py"
SPEC = importlib.util.spec_from_file_location("mcu_compare", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
compare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare)


class MaskWindow(unittest.TestCase):
    def test_x_masked_in_reset_window(self) -> None:
        lines, masked, report = compare.compare(["0 xxxx 00"], ["0 0000 00"])
        self.assertEqual((lines, masked, report), (1, 1, None))

    def test_x_after_reset_window_is_a_mismatch(self) -> None:
        late = compare.MASK_CYCLES
        _, _, report = compare.compare([f"{late} xxxx 00"], [f"{late} 0000 00"])
        self.assertIsNotNone(report)

    def test_real_difference_in_window_still_fails(self) -> None:
        _, _, report = compare.compare(["1 00ff 00"], ["1 00fe 00"])
        self.assertIsNotNone(report)


class ExitStatus(unittest.TestCase):
    def run_compare(self, ref: str, out: str, *extra: str) -> int:
        with tempfile.TemporaryDirectory() as tmp:
            a, b = Path(tmp, "ref.txt"), Path(tmp, "out.txt")
            a.write_text(ref)
            b.write_text(out)
            cmd = [sys.executable, "-I", str(SCRIPT), str(a), str(b), *extra]
            return subprocess.run(cmd, capture_output=True, text=True, check=False).returncode

    def test_identical(self) -> None:
        self.assertEqual(self.run_compare("0 a\n1 b\n", "0 a\n1 b\n", "2"), 0)

    def test_empty_traces_are_an_error(self) -> None:
        self.assertEqual(self.run_compare("", ""), 2)

    def test_fewer_lines_than_expected_is_an_error(self) -> None:
        self.assertEqual(self.run_compare("0 a\n1 b\n", "0 a\n1 b\n", "3"), 2)


if __name__ == "__main__":
    unittest.main()
