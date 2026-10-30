"""V1.2.0 Task 9, step 0 (spec 4.5, "Round trip", claim 2): is each reader/writer pair idempotent?

For every micro design, run the VTR flow to `lower`, write the netlist (`write_blif --vtr`,
`write_json`, `write_verilog`), read it back with the paired reader (`read_blif`, `read_json`,
`read_verilog -netlist` followed by `proc`, which the Verilog writer needs) and write it again. A
pair passes on a micro when the second file equals the first byte for byte (node order, `.latch`
initial values, parameters, ports). The committed list tests/tools/data/restore/step0.txt is the
result; a pair that fails on a micro keeps neither claim for that micro (no netlist claim, no map
claim: the round-trip tests read this list). This test only measures; it does not fix a reader or
a writer. ODIN3_UPDATE_GOLDEN=1 rewrites the list. Needs ODIN3_CLI.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

from .restore_common import (
    DATA,
    LIB,
    MICRO_DIR,
    PAIRS,
    READ,
    UPDATE,
    WRITE,
    flow,
    micros,
    ok,
    run_cli,
    workdir,
)

LIST = DATA / "step0.txt"

# The passes per pair the list must hold (V1.2.0 fix round: write_json numbers its nets the way
# the reader makes them, write_verilog writes the reachable modules and the cells in the reader's
# order).
EXPECTED = {"blif": 68, "json": 68, "v": 68}
# Why each micro that fails a pair still fails it (one line each): none now.
REASONS: dict[str, str] = {}
# The failure signature of each remaining micro: the kind of its first differing line between
# write 1 and write 2 (failure_kind), so a micro that starts failing for another reason is noticed.
SIGNATURES: dict[str, str] = {}


ALIAS_ASSIGN = re.compile(r"^\s*assign \S+ = [^\s{]\S* ?;$")


def failure_kind(first: str, second: str) -> str:
    """The kind of the first line where two writes differ: 'location' (only the trailing
    `// file:line` differs), 'extra-wire' (write 2 declares a wire), 'alias-assign' (write 2 has a
    bare `assign a = b;` where write 1 has another line), 'rewritten-line' (any other change), or
    'length' (one file is a prefix of the other)."""
    a, b = first.splitlines(), second.splitlines()
    for x, y in zip(a, b, strict=False):
        if x == y:
            continue
        if x.split("  // ")[0] == y.split("  // ")[0]:
            return "location"
        if "wire " in y and y.lstrip().startswith(("wire ", "(*")) and "assign" not in y:
            return "extra-wire"
        if ALIAS_ASSIGN.match(y):
            return "alias-assign"
        return "rewritten-line"
    return "length"


FAIL_KINDS: dict[str, str] = {}


def pair_result(micro: str, work: Path) -> dict[str, str]:
    """pass/fail per pair for one micro ('error' when a step could not run)."""
    first = {p: work / f"{micro}.1.{p}" for p in PAIRS}
    script = flow(micro) + "".join(
        WRITE[p].format(names="", out=first[p]) + "; " for p in PAIRS
    ).rstrip("; ")
    if run_cli(script).returncode != 0:
        return {p: "error" for p in PAIRS}
    result = {}
    for pair in PAIRS:
        second = work / f"{micro}.2.{pair}"
        script = f"read_techlib {LIB}; "
        script += READ[pair].format(rmap="", src=first[pair])
        script += WRITE[pair].format(names="", out=second)
        if not ok(script):
            result[pair] = "error"
        else:
            same = first[pair].read_bytes() == second.read_bytes()
            result[pair] = "pass" if same else "fail"
            if pair == "v" and not same:
                kind = failure_kind(first[pair].read_text(), second.read_text())
                FAIL_KINDS[micro] = kind
    return result


def table(rows: dict[str, dict[str, str]]) -> str:
    lines = [f"{m} " + " ".join(f"{p}={rows[m][p]}" for p in PAIRS) for m in sorted(rows)]
    return "\n".join(lines) + "\n"


class Step0(unittest.TestCase):
    def test_step0_idempotence_of_the_three_pairs_on_every_micro(self) -> None:
        work = workdir(self, "step0")
        FAIL_KINDS.clear()
        rows = {m: pair_result(m, work) for m in micros()}
        if not UPDATE:
            self.assertEqual(SIGNATURES, FAIL_KINDS, "first differing line kind per failing micro")
        text = table(rows)
        totals = {p: sum(1 for m in rows if rows[m][p] == "pass") for p in PAIRS}
        print(f"\nstep 0 ({len(rows)} micros, passes per pair): {totals}; files in {work}")
        if UPDATE:
            DATA.mkdir(parents=True, exist_ok=True)
            LIST.write_text(text)
        self.assertEqual(LIST.read_text(), text, "tests/tools/data/restore/step0.txt differs")

    def test_the_committed_list_has_the_expected_passes_and_every_failure_a_reason(self) -> None:
        rows = {}
        for line in LIST.read_text().splitlines():
            name, *cells = line.split()
            rows[name] = dict(c.split("=") for c in cells)
        for pair, count in EXPECTED.items():
            passing = sorted(m for m in rows if rows[m][pair] == "pass")
            self.assertEqual(count, len(passing), f"passes of {pair}")
        failing = sorted(m for m in rows if any(v != "pass" for v in rows[m].values()))
        self.assertEqual(sorted(REASONS), failing, "a failing micro needs a one-line reason")

    def test_the_list_covers_every_micro(self) -> None:
        names = {line.split()[0] for line in LIST.read_text().splitlines()}
        self.assertEqual(names, set(micros()), f"micro dir {MICRO_DIR}")


if __name__ == "__main__":
    unittest.main()
