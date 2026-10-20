"""Plan Task 9 steps 3 and 6 through the CLI (3B spec 12.1, 14.6): the Phase 3 script on a design
with hard blocks next to soft cells, and two runs giving byte-identical ``write_rtlil`` and
``write_blif``. Needs ``ODIN3_CLI``; no Yosys. Work directories are removed when each test ends."""

from __future__ import annotations

import os
import subprocess
import unittest
from pathlib import Path

from . import helpers

ODIN3 = os.environ.get("ODIN3_CLI", "")
ROOT = Path(__file__).resolve().parents[2]
TECHLIB = ROOT / "lib" / "vtr.o3lib"
HAND = ROOT / "tests" / "lower" / "hand"
SCRIPT = "{read}; hierarchy; proc; opt; {pre}lower; opt -fast; compact; check; stats{write}"


def run(
    script: str, cwd: Path, env: dict[str, str] | None = None, prefix: tuple[str, ...] = ()
) -> str:
    done = subprocess.run(
        [*prefix, ODIN3, "--check", "-p", script],
        capture_output=True,
        text=True,
        cwd=cwd,
        check=False,
        timeout=300,
        env=None if env is None else {**os.environ, **env},
    )
    text = done.stdout + done.stderr
    if done.returncode != 0:
        raise AssertionError(f"odin3 exit {done.returncode}: {text[-400:]}")
    return text


@unittest.skipUnless(ODIN3 and Path(ODIN3).exists(), "odin3 (ODIN3_CLI) is needed")
class HardAndSoftTest(unittest.TestCase):
    def test_hard_cells_survive_and_soft_cells_go(self) -> None:
        read = f"read_techlib {TECHLIB}; read_verilog --nolatches {HAND / 'hard_and_soft.v'}"
        work = helpers.scratch(self, "test-lower-script-")
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
        work = helpers.scratch(self, "test-lower-det-")
        storage = any(key in source.name for key in ("dff", "mem", "dlatch", "hard"))
        read = (
            f"read_techlib {TECHLIB}; " if techlib else ""
        ) + f"read_verilog --nolatches {source}"
        write = (
            "" if storage else f"; write_rtlil {work / 'o.il'}"
        ) + f"; write_blif {work / 'o.blif'}"
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
        names = sorted(
            {
                found[0].name
                for prefix in (
                    "add_w5",
                    "shl_w5",
                    "shiftx_w4",
                    "mul_w3",
                    "div_w3",
                    "mod_w3",
                    "sdffe_w4",
                    "adffe_w4",
                    "dffsre_w4",
                    "mem_w4_d4_r2",
                    "dlatch_w4",
                    "pmux",
                    "lt_w5",
                )
                if (found := sorted(cells.glob(f"{prefix}*.v")))
            }
        )
        self.assertGreaterEqual(len(names), 8)
        for name in names:
            with self.subTest(design=name):
                self.assertEqual(
                    self.outputs(cells / name, False), self.outputs(cells / name, False)
                )


@unittest.skipUnless(ODIN3 and Path(ODIN3).exists(), "odin3 (ODIN3_CLI) is needed")
class MixedDesignDeterminismTest(unittest.TestCase):
    """3B-4 review M4: the earlier fixtures are single-cell modules, where nothing can reorder.
    ``mixed_modules.v`` has three modules with arithmetic, shifts, a `$pmux`, flip-flops with
    reset and enable and a `$mem`. The runs differ in their allocator filling and their address
    layout, and the snapshots are compared as well as the final BLIF."""

    SOURCE = HAND / "mixed_modules.v"
    VARIANTS: tuple[tuple[dict[str, str], tuple[str, ...]], ...] = (
        ({}, ()),
        ({"MALLOC_PERTURB_": "165"}, ()),
        ({}, ("setarch", "-R")),
    )

    def one(self, env: dict[str, str], prefix: tuple[str, ...]) -> dict[str, bytes]:
        work = helpers.scratch(self, "test-lower-mixed-")
        script = (
            f"read_verilog --nolatches {self.SOURCE}; hierarchy; proc; opt; "
            f"lower --lower-snapshot {work}; opt -fast; compact; check; "
            f"write_blif {work / 'o.blif'}"
        )
        run(script, work, env, prefix)
        return {
            name: (work / name).read_bytes()
            for name in ("o.blif", "lower-before.il", "lower-after.blif")
        }

    def test_every_variant_gives_identical_outputs_and_snapshots(self) -> None:
        baseline = self.one(*self.VARIANTS[0])
        self.assertTrue(all(baseline.values()))
        self.assertIn(b"module", baseline["lower-before.il"])
        for env, prefix in self.VARIANTS[1:]:
            with self.subTest(env=env, prefix=prefix):
                self.assertEqual(self.one(env, prefix), baseline)


@unittest.skipUnless(ODIN3 and Path(ODIN3).exists(), "odin3 (ODIN3_CLI) is needed")
class SnapshotTest(unittest.TestCase):
    """3B-4 review M3: ``--lower-snapshot DIR`` promises (1) no change to the result and (2)
    identical snapshots across runs."""

    def result(self, name: str, techlib: bool, snap: str | None) -> tuple[bytes, Path]:
        work = helpers.scratch(self, "test-lower-snap-")
        read = (
            f"read_techlib {TECHLIB}; " if techlib else ""
        ) + f"read_verilog --nolatches {HAND / name}"
        option = (
            f" --lower-snapshot {work / 'snap'}"
            if snap == "yes"
            else f" --lower-snapshot {snap}"
            if snap
            else ""
        )
        if snap == "yes":
            (work / "snap").mkdir()
        script = SCRIPT.format(read=read, pre="", write=f"; write_blif {work / 'o.blif'}")
        run(script.replace("lower;", f"lower{option};"), work)
        return (work / "o.blif").read_bytes(), work / "snap"

    def test_the_snapshot_does_not_change_the_result_and_is_reproducible(self) -> None:
        for name, techlib in (("and_mux.v", False), ("hard_and_soft.v", True)):
            with self.subTest(design=name):
                plain, _ = self.result(name, techlib, None)
                first, snap1 = self.result(name, techlib, "yes")
                second, snap2 = self.result(name, techlib, "yes")
                self.assertEqual(plain, first)
                self.assertEqual(plain, second)
                for file in ("lower-before.il", "lower-after.blif"):
                    self.assertTrue((snap1 / file).read_bytes().strip())
                    self.assertEqual((snap1 / file).read_bytes(), (snap2 / file).read_bytes())

    def test_an_unwritable_snapshot_directory_is_a_warning_and_not_a_failure(self) -> None:
        missing = helpers.scratch(self, "test-lower-nosnap-") / "no" / "such" / "dir"
        work = helpers.scratch(self, "test-lower-snap-")
        read = f"read_verilog --nolatches {HAND / 'and_mux.v'}"
        script = SCRIPT.format(read=read, pre="", write="").replace(
            "lower;", f"lower --lower-snapshot {missing};"
        )
        text = run(script, work)
        self.assertIn("warning", text.lower())
        self.assertFalse(missing.exists())
        self.assertIn("lower:", text)


if __name__ == "__main__":
    unittest.main()
