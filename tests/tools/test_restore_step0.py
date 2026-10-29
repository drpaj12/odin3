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
EXPECTED = {"blif": 68, "json": 68, "v": 58}
# Why each micro that fails a pair still fails it (one line each).
REASONS = {
    "bm_DL_BCD_adder": "v: write 1 connects constants as literals, the reader makes named nets",
    "bm_expr_all_mod": "v: write 2 declares extra `$n<ID>$1` wires that write 1 does not",
    "bm_functional_test": "v: write 2 refers to a shared sign bit by another wire (`$n1403`)",
    "bm_if_collapse": "v: the reader folds `x ^ 1'b0` and `x | 1'b0` into an alias assign",
    "bm_if_common": "v: the reader folds a constant operand into an alias assign",
    "bm_stmt_all_mod": "v: the reader folds a constant operand into an alias assign",
    "bm_stmt_compare_padding": "v: the reader folds a constant operand into an alias assign",
    "bm_tester": "v: the reader folds `x ^ 1'b0` into an alias assign",
    "case_generate": "v: the // file:line of a constant assign becomes line 1 on the second read",
    "if_generate": "v: the // file:line of a constant assign becomes line 1 on the second read",
}


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
    return result


def table(rows: dict[str, dict[str, str]]) -> str:
    lines = [f"{m} " + " ".join(f"{p}={rows[m][p]}" for p in PAIRS) for m in sorted(rows)]
    return "\n".join(lines) + "\n"


class Step0(unittest.TestCase):
    def test_step0_idempotence_of_the_three_pairs_on_every_micro(self) -> None:
        work = workdir(self, "step0")
        rows = {m: pair_result(m, work) for m in micros()}
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
