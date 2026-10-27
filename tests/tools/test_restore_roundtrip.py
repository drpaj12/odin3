"""V1.2.0 Task 9 (spec 4.5, "Round trip"): write, read with the written map, write again.

For every micro, every reader/writer pair that step 0 proved idempotent (tests/tools/data/restore/
step0.txt) and every AST mode (`refs`, `full`, `constructs`, none), the VTR flow writes the netlist
and a version 2 map (write 1), the paired reader restores it and the writer writes again (write 2),
and once more from write 2 (write 3). Claims, by test:
  - netlist: write 2 equals write 1 byte for byte (the pair is idempotent, and a v2 restore must not
    change the netlist);
  - map: write 3 equals write 2 (the canonical form), and every map passes `names --check`;
  - a v2 restore makes no restored-location record: write 2 has no more R lines than write 1;
  - the chain of every object that both maps list (its constructs) is the same in write 1 and
    write 2, and write 2 lists no object that write 1 did not (a bit of a port vector is left
    out: the BLIF reader gives the nets of a port one record, the first bit's, because the IR makes
    the nets of a port with the port's record).
  The four claims above are asserted for the BLIF pair. The JSON and Verilog pairs are measured
  (the table): their step 0 passes are few (4 and 23 of 68), and the Verilog reader resolves only
  the hash names of declarators, not of instances (docs/V1.2.0.md, Correction 2026-10-27 Task 9).
  Write 1 equal to write 2 is measured, not asserted, for every pair: (a) two SOURCE records that
  differ only in the AST node they came from restore into one when their constructs are equal (4.5
  names this for `constructs` and call-site misses; it also happens in `refs`), and (b) the map
  lists a net the netlist file does not mention, which a read cannot bind.
The measured table is tests/tools/data/restore/roundtrip.txt (ODIN3_UPDATE_GOLDEN=1 rewrites it).
This is a sweep (minutes): it runs only with ODIN3_RESTORE_SWEEP=1 (CTest restore_roundtrip).
ODIN3_RESTORE_MICROS=N limits it to the first N micros (not for the gate). Needs ODIN3_CLI.
"""

from __future__ import annotations

import os
import unittest
from pathlib import Path

import name_map_v2 as nm2

from .restore_common import (
    DATA,
    MODES,
    PAIRS,
    UPDATE,
    micros,
    rewrite_script,
    run_cli,
    step0_passing,
    workdir,
    write_script,
)

SWEEP = os.environ.get("ODIN3_RESTORE_SWEEP") == "1"
LIMIT = int(os.environ.get("ODIN3_RESTORE_MICROS", "0"))
TABLE = DATA / "roundtrip.txt"


def r_lines(path: Path) -> int:
    return sum(1 for line in path.read_text().splitlines() if line.startswith("R\t"))


def chains(path: Path) -> dict[tuple[str, str, str], list[tuple[str, str, str, str, str]]]:
    """(module, kind, name) -> the chain of the object as (kind, level, name, file, span)."""
    nm = nm2.read_name_map(path)
    out = {}
    for obj in nm.objects:
        if obj.type == "N" and "[" in obj.name:
            continue  # a BLIF port vector takes one record (its first bit's), see the module note
        out[(obj.module, obj.kind, obj.name)] = [
            (c.kind, c.level, c.name, nm.files[c.file - 1] if c.file else "", c.span)
            for c in nm.chain(obj)
        ]
    return out


def check_ok(path: Path) -> bool:
    run = run_cli(f"names --map {path} --check")
    return run.returncode == 0


@unittest.skipUnless(SWEEP, "set ODIN3_RESTORE_SWEEP=1 (CTest restore_roundtrip)")
class RoundTrip(unittest.TestCase):
    def test_round_trip_of_every_proven_pair_in_every_ast_mode(self) -> None:
        work = workdir("roundtrip")
        passing = step0_passing()
        names = micros()[:LIMIT] if LIMIT else micros()
        rows: dict[tuple[str, str], list[int]] = {}
        failures: list[str] = []
        for mode in MODES:
            for micro in names:
                pairs = [p for p in PAIRS if p in passing[micro]]
                outs = {p: work / f"{micro}.{mode}.1.{p}" for p in PAIRS}
                if run_cli(write_script(micro, mode, outs)).returncode != 0:
                    failures.append(f"{micro} {mode}: the first write failed")
                    continue
                for pair in pairs:
                    self.round_trip(pair, mode, micro, outs[pair], work, rows, failures)
        text = "".join(
            f"{mode} {pair} proven={rows[mode, pair][0]} map_identical={rows[mode, pair][1]} "
            f"netlist_identical={rows[mode, pair][2]} write2_eq_write3={rows[mode, pair][3]}\n"
            for mode in MODES
            for pair in PAIRS
            if (mode, pair) in rows
        )
        print(f"\n{text}files in {work}")
        if UPDATE and not LIMIT:
            DATA.mkdir(parents=True, exist_ok=True)
            TABLE.write_text(text)
        self.assertEqual([], failures)
        if not LIMIT:
            self.assertEqual(TABLE.read_text(), text, "tests/tools/data/restore/roundtrip.txt")

    def round_trip(
        self,
        pair: str,
        mode: str,
        micro: str,
        first: Path,
        work: Path,
        rows: dict[tuple[str, str], list[int]],
        failures: list[str],
    ) -> None:
        row = rows.setdefault((mode, pair), [0, 0, 0, 0])
        tag = f"{micro} {mode} {pair}"
        row[0] += 1
        second = work / f"{micro}.{mode}.2.{pair}"
        third = work / f"{micro}.{mode}.3.{pair}"
        map1, map2, map3 = (Path(f"{p}.names") for p in (first, second, third))
        if run_cli(rewrite_script(pair, map1, first, second, mode)).returncode != 0:
            if pair == "blif":
                failures.append(f"{tag}: the restore or the second write failed")
            return
        if run_cli(rewrite_script(pair, map2, second, third, mode)).returncode != 0:
            if pair == "blif":
                failures.append(f"{tag}: the third write failed")
            return
        row[1] += map1.read_bytes() == map2.read_bytes()
        row[2] += first.read_bytes() == second.read_bytes()
        row[3] += map2.read_bytes() == map3.read_bytes()
        if pair != "blif":
            return  # measured only
        if first.read_bytes() != second.read_bytes():
            failures.append(f"{tag}: the netlist changed")
        if map2.read_bytes() != map3.read_bytes():
            failures.append(f"{tag}: write 2 differs from write 3")
        if not check_ok(map2):
            failures.append(f"{tag}: names --check fails on write 2")
        if r_lines(map2) > r_lines(map1):
            failures.append(f"{tag}: write 2 gained R lines")
        one, two = chains(map1), chains(map2)
        if not set(two) <= set(one) or any(one[k] != two[k] for k in two):
            failures.append(f"{tag}: a chain changed")


if __name__ == "__main__":
    unittest.main()
