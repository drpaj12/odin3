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


def flow(
    work: Path, design: str, top: str, abc: str = "abc --lut 6", env: dict[str, str] | None = None
) -> list[list[str]]:
    """Runs the Verilog flow on a fixture; returns the TAB-split lines of the name map."""
    out = work / f"{top}.blif"
    script = (
        f"read_techlib {ROOT}/lib/vtr.o3lib; "
        f'read_verilog --nolatches --vtr-compat --prov-ast refs "{FIX / design}"; '
        + FLOW.format(abc=abc, out=out)
    )
    done = subprocess.run(
        [str(ODIN3), "--top", top, "-p", script],
        capture_output=True,
        text=True,
        cwd=ROOT,
        env=dict(os.environ, **(env or {})),
    )
    assert done.returncode == 0, done.stderr
    return [ln.split("\t") for ln in (work / f"{top}.blif.names").read_text().splitlines()]


def records(rows: list[list[str]]) -> dict[str, list[str]]:
    return {p[1]: p for p in rows if p[0] == "R"}


def flags(p: list[str]) -> list[str]:
    return p[9].split(",") if len(p) > 9 and p[9] else []


def lut_cells(rows: list[list[str]]) -> list[tuple[str, list[str]]]:
    """For each LUT (an O line of level lut): its cut flag and the sorted cell types of the first
    records on its parent chain that name a cell."""
    rec = records(rows)
    out = []
    for p in rows:
        # an O line has recs and level at 8 and 9, an N line (a net that keeps its name) at 4, 5
        at = {"O": 8, "N": 4}.get(p[0])
        if at is not None and len(p) > at + 1 and p[at + 1] == "lut":
            todo, seen, cells = [p[at]], set(), []
            while todo:
                rid = todo.pop()
                if rid in seen or rid not in rec:
                    continue
                seen.add(rid)
                kind = next((f for f in flags(rec[rid]) if f.startswith("cell=")), None)
                if kind:
                    cells.append(kind[5:])
                else:
                    todo += [q for q in rec[rid][6].split(",") if q]
            cut = next((f for f in flags(rec[p[at]]) if f.startswith("cut=")), "")
            out.append((cut, sorted(cells)))
    return out


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


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest prov_cut)")
class Cut(unittest.TestCase):
    def test_exact_cuts_of_muxtest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            luts = lut_cells(flow(Path(tmp), "muxtest.v", "muxtest", "abc --lut 6 --prov-cut on"))
        self.assertEqual(14, len(luts))
        self.assertTrue(all(cut == "cut=exact" for cut, _ in luts), luts)
        by_cells = sorted({tuple(cells) for _, cells in luts})
        self.assertIn(("$and",), by_cells)  # z <= a & c
        self.assertIn(("$mux",), by_cells)  # y <= s ? a : b: the mux alone
        self.assertIn(("$eq", "$mux"), by_cells)  # w <= (a == b) ? c : d: compare and mux

    def test_netlist_is_unchanged(self) -> None:
        def body(path: Path) -> list[str]:
            return [ln for ln in path.read_text().splitlines() if not ln.startswith("# abc:")]

        with tempfile.TemporaryDirectory() as tmp:
            a, b = Path(tmp) / "a", Path(tmp) / "b"
            a.mkdir()
            b.mkdir()
            flow(a, "muxtest.v", "muxtest")
            flow(b, "muxtest.v", "muxtest", "abc --lut 6 --prov-cut on")
            self.assertEqual(body(a / "muxtest.blif"), body(b / "muxtest.blif"))

    def test_without_the_option_nothing_is_labelled(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            luts = lut_cells(flow(Path(tmp), "muxtest.v", "muxtest"))
        self.assertTrue(all(cut == "" for cut, _ in luts), luts)

    def test_both_exchanges_give_the_same_exact_cuts(self) -> None:
        found = []
        for exchange in ("blif", "aig"):
            with tempfile.TemporaryDirectory() as tmp:
                luts = lut_cells(
                    flow(Path(tmp), "muxtest.v", "muxtest", "abc --lut 6 --prov-cut on",
                         {"ODIN3_ABC_EXCHANGE": exchange})
                )
            self.assertEqual({"cut=exact"}, {cut for cut, _ in luts}, exchange)
            found.append(sorted(tuple(cells) for _, cells in luts))
        self.assertEqual(found[0], found[1])

    def test_bad_option_value(self) -> None:
        script = (
            f"read_techlib {ROOT}/lib/vtr.o3lib; read_verilog {FIX / 'case2.v'}; "
            "proc; lower; abc --prov-cut maybe"
        )
        done = subprocess.run([str(ODIN3), "-p", script], capture_output=True, text=True)
        self.assertNotEqual(0, done.returncode)
        self.assertIn("--prov-cut must be on or off", done.stderr)


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest prov_cut)")
class PerInstance(unittest.TestCase):
    def test_flatten_gives_each_instance_its_own_cell_record(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            rows = flow(Path(tmp), "inst2.v", "inst2", "abc --lut 6 --prov-cut on")
        rec = records(rows)
        muxes = [p for p in rec.values() if "cell=$mux" in flags(p)]
        self.assertEqual(2, len(muxes))
        # each hangs on a context record that names its instance path
        paths = set()
        for mux in muxes:
            parent = rec[mux[6]]
            self.assertIn("ctx=1", flags(parent))
            paths |= {f for f in flags(parent) if f.startswith("hier=")}
        self.assertEqual({"hier=inst2/u0", "hier=inst2/u1"}, paths)
        # the LUTs of the two instances are told apart: each has an exact cut on one mux record
        luts = lut_cells(rows)
        self.assertEqual(8, len(luts))
        self.assertTrue(all(cells == ["$mux"] and cut == "cut=exact" for cut, cells in luts))
        parents = {p[6] for p in rec.values() if "cut=exact" in flags(p)}
        self.assertEqual({m[1] for m in muxes}, parents)


if __name__ == "__main__":
    unittest.main()
