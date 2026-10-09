"""Tests for odin3-sim-vectors (tools/sim-check/odin3-sim-vectors.c), the simulator's driver.

The binary comes from ODIN3_SIM_VECTORS (set by the CTest run); without it the tests skip.
"""

from __future__ import annotations

import os
import re
import subprocess
import unittest
from pathlib import Path

from . import helpers

DRIVER = os.environ.get("ODIN3_SIM_VECTORS", "")
BLIF = helpers.REPO_ROOT / "tests" / "golden" / "blif"
TECHLIB = helpers.REPO_ROOT / "tests" / "golden" / "techlib"
VTR_LIB = helpers.REPO_ROOT / "lib" / "vtr.o3lib"
LINE = re.compile(r"^(\d+) ([01]+|-) ([01]+|-)$")


def run(*args: str | Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [DRIVER, *map(str, args)], capture_output=True, text=True, check=False, timeout=120
    )


def vectors(stdout: str) -> list[tuple[int, str, str]]:
    """The data lines as (cycle, inputs, outputs); header lines start with '#'."""
    out = []
    for line in stdout.splitlines():
        if line.startswith("#"):
            continue
        m = LINE.match(line)
        assert m is not None, f"malformed line {line!r}"
        out.append((int(m.group(1)), m.group(2), m.group(3)))
    return out


def header(stdout: str) -> list[str]:
    return [line for line in stdout.splitlines() if line.startswith("#")]


@unittest.skipUnless(DRIVER, "ODIN3_SIM_VECTORS not set")
class SimVectorsTest(unittest.TestCase):
    def ok(self, *args: str | Path) -> str:
        res = run(*args)
        self.assertEqual(res.returncode, 0, res.stderr)
        return res.stdout

    def test_header_and_lines(self) -> None:
        out = self.ok(BLIF / "ff.odin.blif", "--seed", "1", "--cycles", "64")
        self.assertEqual(
            header(out)[1:],
            ["# clock dff^clk 0", "# input dff^rst 1", "# input dff^d 1", "# output dff^q 1"],
        )
        self.assertTrue(header(out)[0].startswith("# odin3-sim-vectors "))
        rows = vectors(out)
        self.assertEqual([cyc for cyc, _, _ in rows], list(range(64)))
        for _, ins, outs in rows:
            rst, d = ins[0], ins[1]  # port order; clk excluded
            self.assertEqual(outs, "0" if rst == "1" else d)
        self.assertIn("1", {outs for _, _, outs in rows})

    def test_oracles_agree(self) -> None:
        odin = self.ok(BLIF / "ff.odin.blif", "--seed", "5", "--cycles", "64")
        parmys = self.ok(BLIF / "ff.parmys.blif", "--seed", "5", "--cycles", "64")
        self.assertEqual(vectors(odin), vectors(parmys))

    def test_deterministic_per_seed(self) -> None:
        args = (BLIF / "ff.parmys.blif", "--cycles", "64", "--seed")
        one = self.ok(*args, "1")
        self.assertEqual(one, self.ok(*args, "1"))
        self.assertNotEqual(vectors(one), vectors(self.ok(*args, "2")))

    def test_multiply_36x36_from_vtr_lib(self) -> None:
        fixture = TECHLIB / "multiply.parmys.01.blif"
        out = self.ok(fixture, "--seed", "3", "--cycles", "64", "--techlib", VTR_LIB)
        self.assertEqual(header(out)[1:], ["# input i_a 36", "# input i_b 36", "# output o_out 72"])
        rows = vectors(out)
        self.assertEqual(len(rows), 64)
        for _, ins, outs in rows:
            a, b = int(ins[:36], 2), int(ins[36:], 2)  # each port MSB first
            self.assertEqual(int(outs, 2), a * b)

    def test_adder_from_vtr_lib(self) -> None:
        fixture = TECHLIB / "adder.odin.01.blif"
        out = self.ok("--techlib", VTR_LIB, fixture, "--cycles", "64", "--seed", "4")
        for _, ins, outs in vectors(out):
            total = sum(map(int, ins))  # a b cin
            self.assertEqual(outs, f"{total >> 1}{total & 1}")  # cout sumout

    def test_every_simulatable_fixture_runs_64_cycles(self) -> None:
        for name in [
            "adder_hard_block.parmys.blif",
            "ansiportlist_2.parmys.blif",
            "ff.odin.blif",
            "ff.parmys.blif",
            "hand_ports.blif",
        ]:
            with self.subTest(fixture=name):
                out = self.ok(BLIF / name, "--seed", "1", "--cycles", "64", "--techlib", VTR_LIB)
                self.assertEqual(len(vectors(out)), 64)

    def test_zero_cycles_prints_the_header_only(self) -> None:
        out = self.ok(BLIF / "ff.parmys.blif", "--seed", "1", "--cycles", "0")
        self.assertEqual(vectors(out), [])
        self.assertEqual(len(header(out)), 5)

    def test_usage_errors(self) -> None:
        ff = BLIF / "ff.parmys.blif"
        for args in [
            (),
            (ff,),
            (ff, "--seed", "1"),
            (ff, "--cycles", "4"),
            (ff, "--seed", "x1", "--cycles", "4"),
            (ff, "--seed", "1", "--cycles", "-4"),
            (ff, "--seed", "1", "--cycles", "99999999999"),
            (ff, ff, "--seed", "1", "--cycles", "4"),
            (ff, "--seed", "1", "--cycles", "4", "--bogus"),
            (ff, "--seed", "1", "--cycles"),
        ]:
            with self.subTest(args=args):
                res = run(*args)
                self.assertEqual(res.returncode, 2)
                self.assertIn("usage:", res.stderr)

    def test_failures(self) -> None:
        cases: list[tuple[tuple[str | Path, ...], str]] = [
            ((BLIF / "no_such.blif",), "no_such.blif"),
            ((BLIF / "pow.parmys.blif",), "cannot simulate"),
            ((TECHLIB / "multiply.parmys.01.blif",), "cannot simulate"),
            ((BLIF / "elsif_both_defined.odin.blif",), "driver"),
            ((BLIF / "ff.parmys.blif", "--techlib", Path("/nonexistent.o3lib")), "nonexistent"),
        ]
        for args, needle in cases:
            with self.subTest(args=args):
                res = run(*args, "--seed", "1", "--cycles", "4")
                self.assertEqual(res.returncode, 1, res.stderr)
                self.assertIn(needle, res.stderr)
                self.assertEqual(res.stdout, "")


if __name__ == "__main__":
    unittest.main()
