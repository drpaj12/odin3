---
name: run-micro
description: Use before claiming any micro benchmark or pass "works", at the end of the per-pass workflow, or when the user says /run-micro. Builds the debug preset and compares Odin III output on each tests/micro design with the Parmys golden (netlist-compare, then equiv-check), printing a pass/fail table.
---

# run-micro

Never claim a micro passes unless this skill ran the comparison and you saw the exit codes.

## 1. Build
`cmake --preset debug && cmake --build --preset debug` (ASan+UBSan). Stop and report on build failure. Optionally `ctest --preset debug` first.

## 2. Detect Phase 0 reality
Run `.claude/skills/run-micro/run-micro.sh` (small bash driver, read it first). It checks, in order:
1. `tests/micro/` has `.v` designs (otherwise: "no micro designs yet").
2. `build/debug/odin3` exists.
3. `ODIN3_MICRO_CMD` is set: the exact command for one design, with `{design}`, `{arch}`, `{out}` placeholders (write BLIF to `{out}`). Odin III's CLI for this does not exist yet; set it once known (ask the human).
If any check fails, print the matching message, report **no pass and no fail**, and stop. Do not fabricate output or compare goldens against themselves.

## 3. Compare (script does this per design and arch)
Golden: `~/odin3-ws/golden/<arch>/micro/<leaf>/<leaf>.parmys.blif` for `<arch>` in `EArch`, `k6_frac_N10_frac_chain_mem32K_40nm`. Missing golden => `no golden` (run `/oracle`), not a pass. Odin III output goes to `build/micro-out/<arch>/<leaf>.blif`.
1. `python3 tools/netlist-compare/netlist_compare.py ours.blif golden.blif`: exit 0 identical, 1 different, 2 error.
2. If not identical: `python3 tools/equiv-check/equiv_check.py ours.blif golden.blif`: 0 equivalent, 1 not, 2 error (e.g. latch init, undriven nets; read the message).
3. Odin III failing to produce a BLIF is a FAIL with the log tail as note.

## 4. Report
Table: `design | arch | identical | equivalent | note` with yes/no/skipped/error. Then totals. "Identical" is the Phase 2 target; "equivalent but not identical" is a partial result, say so. Show the netlist-compare diff head for a few differing designs. Script exit: 0 all pass, 1 any fail, 2 a tool errored (an error is neither a pass nor a fail — report it), 3 nothing could be run (Phase 0/1: say so, claim nothing).
