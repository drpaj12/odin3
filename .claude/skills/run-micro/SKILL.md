---
name: run-micro
description: Use before claiming any micro benchmark or pass "works", at the end of the per-pass workflow, or when the user says /run-micro. Builds the debug preset and runs the CTest micro sweeps (hardmap-diff and pm-diff-micro against the Parmys goldens, LPM, FSM, tri-state micros), printing a pass/fail table.
---

# run-micro

Never claim a micro passes unless this skill ran the sweeps and you saw the table and the exit code.

## 1. Build
`cmake --preset debug && tools/build-locked --preset debug -j4` (ASan+UBSan; one build at a time, -j capped at 4). Stop and report on build failure.

## 2. Run
`.claude/skills/run-micro/run-micro.sh` (read it first). It runs the CTest micro sweeps of the current build (`build/debug/odin3`; `PRESET=release` for the release tree):
`hardmap_diff_micro` and `pm_diff_micro_<arch>` (Odin III's pre-ABC netlist against the Parmys golden of each sweep arch, with the expect files under `tests/hardmap` and `tests/partial_map`: identical, equivalent, or a listed difference), `lpm_check_*`, `fsm_equiv_micro`, `fsm_diff_micro`, `tristate_micro`; `MICRO_REGEX` adds more test names. The comparisons need `VTR_ROOT` (defaulted to `~/odin3-ws/external/vtr-verilog-to-routing`) and the golden tree `~/odin3-ws/golden` (run `/oracle` for a missing design); a test that cannot run is reported as skipped, never as passed.

## 3. Report
Table `test | result` (pass / FAIL / skipped), then the totals. For a failure the script prints the tail of the ctest output; show the first differing design. Exit: 0 all pass, 1 any fail, 3 nothing ran (no binary, no tests, or everything skipped): say so, claim nothing. "Identical" is the Phase 2 target; an expected-difference entry in an expect file is a documented, not a new, difference.

The whole suite is `ctest --preset debug`; the micro-only run is this script.
