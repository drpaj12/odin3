"""V1.2.0 Task 9 (spec 4.5, "Round trip"): write, read with the written map, write again.

For every micro, every reader/writer pair that step 0 proved idempotent (tests/tools/data/restore/
step0.txt) and every AST mode (`refs`, `full`, `constructs`, none), the VTR flow writes the netlist
and a version 2 map (write 1), the paired reader restores it and the writer writes again (write 2),
and once more from write 2 (write 3). Claims, by test:
  - netlist: write 2 equals write 1 byte for byte (the pair is idempotent, and a v2 restore must not
    change the netlist);
  - map: write 3 equals write 2 (the canonical form), and every map passes `names --check`;
  - write 2 gains no R line, and loses none unless an object was lost or the map has a port vector
    (compare_maps); every object that both maps list has the same chain (the bits of a port vector
    take the chain of bit 0); write 2 lists no object write 1 did not; an object write 2 lost is
    one the netlist does not mention (the bits of a port vector are a known gap: the BLIF reader
    binds a port through its first bit, which may have no line).
  The claims above are asserted for the BLIF pair. The JSON and Verilog pairs are measured
  (the table): their step 0 passes are few (4 and 23 of 68).
  Write 1 equal to write 2 is measured, not asserted, for every pair: (a) two SOURCE records that
  differ only in the AST node they came from restore into one when their constructs are equal (4.5
  names this for `constructs` and call-site misses; it also happens in `refs`), and (b) the map
  lists a net the netlist file does not mention, which a read cannot bind. For the Verilog pair
  the cause is that the reader binds no records to cells or modules (the pair loses the
  `// file:line` comments of the module and the assign); it is not the instance hash names.
The measured table is tests/tools/data/restore/roundtrip.txt (ODIN3_UPDATE_GOLDEN=1 rewrites it).
This is a sweep (minutes): it runs only with ODIN3_RESTORE_SWEEP=1 (CTest restore_roundtrip).
ODIN3_RESTORE_MICROS=N limits it to the first N micros (not for the gate). Needs ODIN3_CLI.
"""

from __future__ import annotations

import os
import re
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


# The cause of every cell that is below `proven` (a "known gap"), by pair and column.
GAPS = {
    ("blif", 1): "known gap: a port vector keeps the record of its first bit and the map lists "
    "objects the netlist does not mention, so write 2 has fewer N and R lines",
    ("v", 1): "known gap: the Verilog reader binds no records to cells or modules, so write 2 "
    "loses the // file:line comments of the module and the assign and its map holds new I records",
    ("v", 2): "known gap: the same cause, the netlist differs in the // file:line comments",
    ("v", 3): "known gap: the same cause, each Verilog read imports locations again",
}
COLUMNS = ("map_identical", "netlist_identical", "write2_eq_write3")


def table_row(mode: str, pair: str, row: list[int]) -> str:
    """One line of the table; every column below `proven` is followed by its known gap."""
    cells = []
    for index, column in enumerate(COLUMNS, start=1):
        cell = f"{column}={row[index]}"
        if row[index] < row[0]:
            cell += f" ({GAPS.get((pair, index), 'UNEXPLAINED')})"
        cells.append(cell)
    return f"{mode} {pair} proven={row[0]} " + " ".join(cells) + "\n"


def r_lines(path: Path) -> int:
    return sum(1 for line in path.read_text().splitlines() if line.startswith("R\t"))


Chain = list[tuple[str, str, str, str, str]]
Key = tuple[str, str, str]


def chains(path: Path) -> dict[Key, Chain]:
    """(module, kind, name) -> the chain of the object as (kind, level, name, file, span). Every
    object is in it, the bits of a port vector too."""
    nm = nm2.read_name_map(path)
    out = {}
    for obj in nm.objects:
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
            table_row(mode, pair, rows[mode, pair])
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
        failures.extend(compare_maps(tag, map1, map2, first))


def vector_bit0(key: Key) -> Key | None:
    """The key of bit 0 of the port vector a `name[k]` key is a bit of; None for a scalar."""
    module, kind, name = key
    if not name.endswith("]") or "[" not in name:
        return None
    return (module, kind, name[: name.rindex("[")] + "[0]")


def compare_maps(tag: str, map1: Path, map2: Path, netlist: Path) -> list[str]:
    """What write 2 must keep of write 1 (the BLIF pair). Objects in both: the same chain, except
    the bits of a port vector (the IR gives the nets of a port the port's record, the first bit's,
    docs/V1.2.0.md Task 9 default 4): every bit takes the chain bit 0 had in write 1. Objects in
    write 1 only: the netlist file must not mention their name (a read cannot bind them; the
    bits of a port vector after the first are a known gap and exempt), and nothing is in write 2
    only. R lines are lost only with such objects."""
    problems: list[str] = []
    one, two = chains(map1), chains(map2)
    if not set(two) <= set(one):
        problems.append(f"{tag}: write 2 lists an object write 1 did not")
    words = set(re.split(r"[ \t\n=]+", netlist.read_text().replace("\\\n", " ")))
    missing = [k for k in one if k not in two]
    for key in missing:
        if vector_bit0(key) is not None:
            continue  # known gap: a port is bound through its first bit, which may have no line
        if key[2] in words:
            problems.append(f"{tag}: the netlist mentions {key} but write 2 lost it")
    for key in two:
        bit0 = vector_bit0(key)
        allowed = [one[key]] + ([one[bit0]] if bit0 is not None and bit0 in one else [])
        if two[key] not in allowed:
            problems.append(f"{tag}: the chain of {key} changed")
    vectors = any(vector_bit0(k) is not None for k in one)
    gained, kept = r_lines(map2) - r_lines(map1), not missing and not vectors
    if gained > 0:
        problems.append(f"{tag}: write 2 gained R lines")
    if gained < 0 and kept:
        problems.append(f"{tag}: write 2 lost R lines with every object kept and no port vector")
    return problems


if __name__ == "__main__":
    unittest.main()
