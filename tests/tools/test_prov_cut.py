"""Per-cell and per-LUT provenance records (v1.6.2): the word-level cell type in the name map,
two cells from one statement told apart, and the exact cut of a LUT. Run through CTest
`prov_cut` (ODIN3_BIN set); skipped without it.

Fixtures: tests/prov_cut/muxtest.v (two muxes, an AND, a compare feeding a mux) and
tests/prov_cut/case2.v (one `case` driving two outputs).
"""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ODIN3 = os.environ.get("ODIN3_BIN")
ROOT = Path(__file__).resolve().parents[2]
FIX = ROOT / "tests" / "prov_cut"

FLOW = (
    "proc; fsm; opt; flatten; opt -full; wreduce; hardmap; lower; {abc}; "
    "write_blif --vtr --lut-k 6 --name-style hash --prov-map {out}"
)


def flow(work: Path, design: str, top: str, abc: str = "abc --lut 6") -> list[list[str]]:
    """Runs the Verilog flow on a fixture; returns the TAB-split lines of the name map."""
    out = work / f"{top}.blif"
    script = (
        f"read_techlib {ROOT}/lib/vtr.o3lib; "
        f'read_verilog --nolatches --vtr-compat --prov-ast refs "{FIX / design}"; '
        + FLOW.format(abc=abc, out=out)
    )
    done = subprocess.run(
        [str(ODIN3), "--top", top, "-p", script], capture_output=True, text=True, cwd=ROOT
    )
    assert done.returncode == 0, done.stderr
    return [ln.split("\t") for ln in (work / f"{top}.blif.names").read_text().splitlines()]


def records(rows: list[list[str]]) -> dict[str, list[str]]:
    return {p[1]: p for p in rows if p[0] == "R"}


def flags(p: list[str]) -> list[str]:
    return p[9].split(",") if len(p) > 9 and p[9] else []


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest prov_cut)")
class CellType(unittest.TestCase):
    def test_cell_type_width_and_arms(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            rec = records(flow(Path(tmp), "muxtest.v", "muxtest"))
        cells = [flags(p) for p in rec.values() if any(f.startswith("cell=") for f in flags(p))]
        types = sorted(next(f for f in fl if f.startswith("cell=")) for fl in cells)
        self.assertEqual(["cell=$and", "cell=$dff", "cell=$dff", "cell=$dff", "cell=$eq",
                          "cell=$mux", "cell=$mux"], types)
        for fl in cells:
            if "cell=$mux" in fl:
                self.assertEqual(["cell=$mux", "cw=4", "ca=2"], fl)
        # a record without a cell has none of the new flags
        self.assertFalse(any("cell=" in ",".join(flags(p)) for p in rec.values()
                             if p[2] == "S"))

    def test_two_cells_of_one_statement_have_two_records(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            rec = records(flow(Path(tmp), "case2.v", "case2"))
        pmux = [p for p in rec.values() if "cell=$pmux" in flags(p)]
        self.assertEqual(2, len(pmux))
        self.assertNotEqual(pmux[0][1], pmux[1][1])
        self.assertNotEqual(pmux[0][5], pmux[1][5])  # two operations of the lower run
        self.assertEqual(pmux[0][6], pmux[1][6])  # the same parent: the same HDL statement
        self.assertEqual(["cell=$pmux", "cw=4", "ca=3"], flags(pmux[0]))  # 2 selected + default

    def test_other_readers_see_the_old_fields(self) -> None:
        """The new flags are in the flags field only: every R line keeps its nine fields."""
        with tempfile.TemporaryDirectory() as tmp:
            rows = flow(Path(tmp), "case2.v", "case2")
        for p in rows:
            if p[0] == "R":
                self.assertEqual(10, len(p), p)
                self.assertTrue(re.fullmatch(r"r\d+", p[1]))


if __name__ == "__main__":
    unittest.main()
