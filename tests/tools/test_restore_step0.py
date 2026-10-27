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
        work = workdir("step0")
        rows = {m: pair_result(m, work) for m in micros()}
        text = table(rows)
        totals = {p: sum(1 for m in rows if rows[m][p] == "pass") for p in PAIRS}
        print(f"\nstep 0 ({len(rows)} micros, passes per pair): {totals}; files in {work}")
        if UPDATE:
            DATA.mkdir(parents=True, exist_ok=True)
            LIST.write_text(text)
        self.assertEqual(LIST.read_text(), text, "tests/tools/data/restore/step0.txt differs")

    def test_the_list_covers_every_micro(self) -> None:
        names = {line.split()[0] for line in LIST.read_text().splitlines()}
        self.assertEqual(names, set(micros()), f"micro dir {MICRO_DIR}")


if __name__ == "__main__":
    unittest.main()
