"""Per-LUT lineage: which source lines a LUT of the final netlist traces back to, through the
provenance records (cells made by `proc`, then every pass, then abc). Run through CTest `lineage`
(ODIN3_BIN set); skipped without it.

The micro tests/lineage/case_fsm.v has a case that drives y (lines 7-11), an independent statement
for o (line 14) and a 3-state machine (lines 15-20) with the outputs p and q (lines 22-23).
Hand-checked: the LUTs of y trace to the case and its items only, the LUT of o to its
statement only, and the LUTs of the machine to the machine's lines only. The join uses `uid=`,
the store ID that ODIN3_PROV_UID=1 adds to the R lines (the r<n> numbers count the records of
one file, so two dumps disagree on them).
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
import unittest
from collections import defaultdict
from pathlib import Path

ODIN3 = os.environ.get("ODIN3_BIN")
ROOT = Path(__file__).resolve().parents[2]
MICRO = ROOT / "tests" / "lineage" / "case_fsm.v"

CASE = {7, 8, 9, 10, 11}  # the case, its three items and its default
OWN = {14}
MACHINE = {15, 17, 18, 19, 20, 22, 23}


def load(path: Path) -> tuple[dict[str, list[str]], dict[str, str], list[list[str]]]:
    """uid -> parent uids, record id -> uid, and the O lines of a version 2 name map."""
    rows = [line.split("\t") for line in path.read_text().splitlines()]
    uid_of = {}
    for p in rows:
        if p[0] == "R" and len(p) > 9:
            m = re.search(r"uid=(\d+)", p[9])
            uid_of[p[1]] = m.group(1) if m else ""
    parents = {}
    for p in rows:
        if p[0] == "R" and len(p) > 6:
            parents[uid_of[p[1]]] = [uid_of[q] for q in p[6].split(",") if q in uid_of]
    return parents, uid_of, [p for p in rows if p[0] == "O" and len(p) > 9]


def lines_of(src: str) -> set[int]:
    return {int(m.group(1)) for part in src.split("|") if (m := re.match(r".*?:(\d+)\.", part))}


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest lineage)")
class Lineage(unittest.TestCase):
    def lut_lines(self) -> list[set[int]]:
        with tempfile.TemporaryDirectory() as tmp:
            w = Path(tmp)
            script = (
                f"read_techlib {ROOT}/lib/vtr.o3lib; "
                f'read_verilog --nolatches --vtr-compat --prov-ast refs "{MICRO}"; proc; '
                f"write_json --odin3 --name-style provenance {w}/proc.json; "
                f"write_json --odin3 --name-style hash --prov-map {w}/proc_h.json; "
                "fsm; opt; flatten; tristate; opt -full; wreduce; hardmap; lower; abc --lut 6; "
                f"write_blif --vtr --lut-k 6 --name-style hash --prov-map {w}/o.blif"
            )
            env = dict(os.environ, ODIN3_PROV_UID="1")
            done = subprocess.run(
                [str(ODIN3), "--top", "lin", "-p", script],
                capture_output=True,
                text=True,
                env=env,
                cwd=ROOT,
            )
            self.assertEqual(0, done.returncode, done.stderr)
            self.assertIn("mapped 'lin.st'", done.stderr)  # the machine was recoded
            cells = json.loads((w / "proc.json").read_text())["modules"]["lin"]["cells"]
            _, uid_of, objs = load(w / "proc_h.json.names")
            cell_uid = {p[4]: uid_of[p[8]] for p in objs if p[2] == "node" and p[8] in uid_of}
            by_uid = defaultdict(list)
            for key, cell in cells.items():
                if cell["type"] != "$dff" and key in cell_uid:
                    by_uid[cell_uid[key]].append(key)
            parents, fuid, fobjs = load(w / "o.blif.names")
            memo: dict[str, set[str]] = {}

            def cells_of(uid: str) -> set[str]:
                if uid not in memo:
                    memo[uid] = set()
                    got = set(by_uid.get(uid, []))
                    for q in parents.get(uid, []):
                        got |= cells_of(q)
                    memo[uid] = got
                return memo[uid]

            out = []
            for p in fobjs:
                if p[9] == "lut":
                    ls: set[int] = set()
                    for key in cells_of(fuid[p[8]]):
                        ls |= lines_of(cells[key].get("attributes", {}).get("src", ""))
                    out.append(ls)
            return out

    def test_luts_trace_to_their_own_lines(self) -> None:
        luts = self.lut_lines()
        self.assertEqual(8, len(luts))
        self.assertEqual(2, sum(1 for ls in luts if ls == CASE), luts)  # the two bits of y
        self.assertEqual(1, sum(1 for ls in luts if ls == OWN), luts)  # o
        machine = [ls for ls in luts if ls not in (CASE, OWN)]
        self.assertEqual(5, len(machine), luts)
        for ls in machine:
            self.assertTrue(ls and ls <= MACHINE, ls)


    def test_graph_and_scoap_prov_ids_are_the_map_uids(self) -> None:
        """The `prov` of a graph node and of a scoap net is the store ID, the `uid=` of the map."""
        with tempfile.TemporaryDirectory() as tmp:
            w = Path(tmp)
            script = (
                f"read_techlib {ROOT}/lib/vtr.o3lib; "
                f'read_verilog --nolatches --vtr-compat --prov-ast refs "{MICRO}"; proc; '
                f"write_graph ddg {w}/ddg.json; write_graph stg {w}/stg.json; "
                f"write_json --name-style hash --prov-map {w}/h.json; "
                f"scoap --prov --out {w}/scoap.json"
            )
            env = dict(os.environ, ODIN3_PROV_UID="1")
            done = subprocess.run(
                [str(ODIN3), "--top", "lin", "-p", script],
                capture_output=True,
                text=True,
                env=env,
                cwd=ROOT,
            )
            self.assertEqual(0, done.returncode, done.stderr)
            uids = set(re.findall(r"uid=(\d+)", (w / "h.json.names").read_text()))
            self.assertTrue(uids)
            for name in ("ddg.json", "stg.json", "scoap.json"):
                provs = set(re.findall(r'"prov": ([1-9]\d*)', (w / name).read_text()))
                self.assertTrue(provs, name)
                if name == "scoap.json":  # the map lists the records of the objects it names
                    self.assertTrue(provs & uids, name)
                else:
                    self.assertLessEqual(provs, uids, name)

if __name__ == "__main__":
    unittest.main()
