"""Plan Task 9 steps 3 and 6 through the CLI (3B spec 12.1, 14.6): the Phase 3 script on a design
with hard blocks next to soft cells, and two runs giving byte-identical ``write_rtlil`` and
``write_blif``. Needs ``ODIN3_CLI``; no Yosys. Work directories are ``tempfile.mkdtemp``, left in
place (these tools never delete)."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ODIN3 = os.environ.get("ODIN3_CLI", "")
ROOT = Path(__file__).resolve().parents[2]
TECHLIB = ROOT / "lib" / "vtr.o3lib"
HAND = ROOT / "tests" / "lower" / "hand"
SCRIPT = "{read}; hierarchy; proc; opt; {pre}lower; opt -fast; compact; check; stats{write}"


def run(script: str, cwd: Path) -> str:
    done = subprocess.run([ODIN3, "--check", "-p", script], capture_output=True, text=True,
                          cwd=cwd, check=False, timeout=300)
    text = done.stdout + done.stderr
    if done.returncode != 0:
        raise AssertionError(f"odin3 exit {done.returncode}: {text[-400:]}")
    return text


@unittest.skipUnless(ODIN3 and Path(ODIN3).exists(), "odin3 (ODIN3_CLI) is needed")
class HardAndSoftTest(unittest.TestCase):
    def test_hard_cells_survive_and_soft_cells_go(self) -> None:
        read = f"read_techlib {TECHLIB}; read_verilog --nolatches {HAND / 'hard_and_soft.v'}"
        work = Path(tempfile.mkdtemp(prefix="test-lower-script-"))
        text = run(SCRIPT.format(read=read, pre="", write=""), work)
        for hard in ("adder", "multiply", "single_port_ram"):
            self.assertIn(f"stats: module top: cell {hard} 1", text)
        for soft in ("$add", "$mem", "$mul"):
            self.assertNotIn(f"cell {soft} ", text)
        self.assertIn("lower: lowered 1 $add, 1 $mem, 1 $mul as soft logic", text)
        self.assertRegex(text, r"lower: module 'top': lowered 6 word cells to \d+ bit cells")


@unittest.skipUnless(ODIN3 and Path(ODIN3).exists(), "odin3 (ODIN3_CLI) is needed")
class DeterminismTest(unittest.TestCase):
    """Two runs, byte-identical outputs (spec 12.1)."""

    def outputs(self, source: Path, techlib: bool) -> tuple[bytes, bytes]:
        """(write_rtlil, write_blif) bytes. The RTLIL view has no form for a storage bit cell
        (1F), so a design with flip-flops or memories is compared by its BLIF alone."""
        work = Path(tempfile.mkdtemp(prefix="test-lower-det-"))
        storage = any(key in source.name for key in ("dff", "mem", "dlatch", "hard"))
        read = (f"read_techlib {TECHLIB}; " if techlib else "") + \
            f"read_verilog --nolatches {source}"
        write = ("" if storage else f"; write_rtlil {work / 'o.il'}") + \
            f"; write_blif {work / 'o.blif'}"
        run(SCRIPT.format(read=read, pre="", write=write), work)
        rtlil = b"" if storage else (work / "o.il").read_bytes()
        return rtlil, (work / "o.blif").read_bytes()

    def test_two_runs_are_byte_identical(self) -> None:
        for name, techlib in (("hard_and_soft.v", True), ("and_mux.v", False)):
            with self.subTest(design=name):
                first = self.outputs(HAND / name, techlib)
                second = self.outputs(HAND / name, techlib)
                self.assertEqual(first, second)
                self.assertTrue(first[1].strip())

    def test_the_cell_fixtures_of_every_family_are_byte_identical(self) -> None:
        cells = ROOT / "tests" / "lower" / "cells"
        names = sorted({found[0].name for prefix in (
            "add_w5", "shl_w5", "shiftx_w4", "mul_w3", "div_w3", "mod_w3", "sdffe_w4", "adffe_w4",
            "dffsre_w4", "mem_w4_d4_r2", "dlatch_w4", "pmux", "lt_w5")
            if (found := sorted(cells.glob(f"{prefix}*.v")))})
        self.assertGreaterEqual(len(names), 8)
        for name in names:
            with self.subTest(design=name):
                self.assertEqual(self.outputs(cells / name, False),
                                 self.outputs(cells / name, False))


if __name__ == "__main__":
    unittest.main()
