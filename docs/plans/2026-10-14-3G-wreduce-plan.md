# 3G `wreduce` — Implementation Plan (v1, approved by Peter 2026-10-14, PHASE3 #31)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port Yosys 0.55's `wreduce` onto the IR so that, on the same input, Odin III narrows the same cells to the same widths and flags (spec WR-1..WR-12), with PHASE2 #25's both-signed rule wherever Yosys reads a flag (WR-6), and put it in the Phase 3 script at Parmys's position (WR-9).

**Architecture:** one pass in the opt family, `src/passes/opt/wreduce.c` (pre-step, worker, §4.3 operator rules) and `wreduce_ff.c` (§4.1 mux and §4.2 FF rules), on 2F's kit: `kit_connect`, `kit_rebuild_node`, constant nets, the opt context and dirty set, `odin3_budget_check`, `ffinit`. The evidence reuses 2F's tools (`opt-rules`, `opt-equiv`, `opt-diff`) with a `wreduce` row set and a Phase 3 script; nothing is forked.

**Tech stack:** C17, `src/util`, `src/ir`, `src/passes/opt/kit.[ch]`, `src/passes/ffinit.[ch]`, `src/passes/budget.[ch]`; Unity; Python 3 stdlib; Yosys 0.55 (`$VTR_ROOT/build/bin/yosys`); `tools/heavy-run`.

**Spec:** `docs/specs/2026-10-14-3G-wreduce-design.md` v1. Binding; WR-n and § numbers below refer to it.

**Branch and PR:** `feat/3G-wreduce` from `main`; one PR (one pass per PR), three tasks in order 1 → 2 → 3. Light review (PHASE3 row 3G); a read-only critique per task, a whole-branch review before the PR. Model/effort: Opus `high` (PHASE3 row 3G); Sonnet `high` for Task 3's bulk rule sources.

## Global constraints

- **Code standard** (CLAUDE.md): C17, `-Werror`, lizard ≤ 15 CCN / 60 lines / 5 parameters, no VLA, no recursion in `src/` (WR-3: a loop replaces `run_cell`'s self-call), only `cleanup:`, named constants, `src/util` containers, no `NOLINT`.
- **IR discipline** (2F OP-2): build before the first destructive step; `NO_MEMORY` leaves the IR valid and the node unchanged; names are never dropped (IR-2); `$proc` modules are skipped (WR-3); every test that builds IR runs `odin3_check_design` FULL.
- **Determinism** (WR-7): node-ID order, deduplicated sorted rounds; two runs byte-identical.
- **Diagnostics:** §11's texts verbatim; tests assert whole lines through a capturing sink.
- **Caps:** `ODIN3_WREDUCE_MAX_ROUNDS` (1,000,000, lowered by a test hook in `opt_test.h`); the design budget through `odin3_budget_check`.
- **Builds:** `tools/build-locked --preset debug -j6 && ctest --preset debug -j4`. No build while a heavy sweep holds the lock; sweeps only through `tools/heavy-run`, smallest first, run by the controller.
- `/lint` before each commit.

## Task 1: Pre-step and operator rules (WR-3 step 1, WR-4 §4.3, WR-5, WR-6)

**Files:**
- Create `src/passes/opt/wreduce.c`, `src/passes/opt/wreduce.h` (internal: `odin3_wreduce_opts`, the counters, `odin3_wreduce_run(ctx, design, &opts)`).
- Modify `src/passes/opt/register.c` (the `wreduce` pass definition, `processes = OK`, options `-memx -keepdc -mux_undef`, the unknown-argument error), `src/passes/opt/opt.h` (counter fields for `--opt-stats`), `CMakeLists.txt`.
- Create `tests/unit/test_wreduce.c`.

- [ ] **Step 1: Write failing tests** (hand-built IR through `pb_build`/`read_blif`-free builders, FULL `check` after each):
  - pre-step: `$eq` with `Y_WIDTH 8` → 1 bit, Y[1..7] on 0; `$div` unsigned A with three top zeros trimmed, signed A keeps one zero; `$pow` reads each flag separately; `$mem` `SIZE 10 OFFSET 0 ABITS 32` → `ABITS 4`, with `-memx` unchanged;
  - `$add` 32-bit of two 8-bit zero-extended operands into an unused-top 32-bit Y → `A 8, B 8, Y 9`, Y[9..31] on 0;
  - the signed rewrite: `sx_add`'s cell (`{x[8],x[8],x} + {y[8],y[8],y}`, 11 bits) → `A_SIGNED=B_SIGNED=1`, A 9, B 9, Y 11 (rule 2 then 3; the pad of rule 6 is Y[10]);
  - `$mul` free choice by product cost; `$sub` pad is the top Y bit even when unsigned;
  - rule 4: both-signed cell with zero tops → unsigned; unary `$neg` signed with zero top → unsigned;
  - rule 5: unused top Y bits removed, except a signed-A `$shr`; `keep` and output-port bits stay;
  - mixed-sign (WR-6): `$add A_SIGNED=1 B_SIGNED=0` trims A as unsigned, pads with 0, warns once with §11's line; `$shl` B always unsigned; `$shiftx` A untouched;
  - constant Y bit → cell skipped; a module with a `$proc` → skipped with the info line;
  - OOM sweep over every allocation of one narrowing (`NO_MEMORY`, IR valid, node unchanged).
- [ ] **Step 2: Run them; they fail** (`ctest --preset debug -R wreduce`).
- [ ] **Step 3: Implement** the pre-step and `run_cell` (§4.3 steps 1–7, the loop in place of recursion), the counters and the DEBUG lines.
- [ ] **Step 4: Run; green.** Then `/lint`, commit `feat(wreduce): pre-step and operator rules (3G WR-3/4/6)`, push.

## Task 2: Mux and FF rules, the worker, determinism and caps (WR-3 step 2, WR-4 §4.1–4.2, WR-7, WR-10)

**Files:**
- Create `src/passes/opt/wreduce_ff.c`.
- Modify `src/passes/opt/wreduce.c` (the worker: keep nets, init load, work list and rounds, the dirty-set marks, the budget check, the round cap), `tests/unit/test_wreduce.c`.

- [ ] **Step 1: Write failing tests:**
  - `$mux`/`$pmux`: agreeing top bits removed (wired to the common net), unused top bits removed (x), whole-cell removal, `-mux_undef` with x inputs, `-keepdc` blocks it;
  - FFs: zero-extended `$dff` with init 0 / x / 1 (only the first two narrow; `-keepdc` blocks x), sign-extended `$sdff` with equal reset bits, `ARST_VALUE` truncated, unused Q bits, a fully removed FF, init removal checked through `odin3_ffinit_get`; `$dlatch` and `$adlatch` too;
  - a cascade needing two rounds (adder → register → adder, the `sx_add` chain), with the round count asserted;
  - determinism: two runs give byte-identical `write_rtlil`; a reversed-creation module gives a `netlist-compare`-identical result;
  - the round cap through the test hook: warning, `ODIN3_OK`, valid IR;
  - the dirty set: after `wreduce`, an `opt -full` with `--opt-full-rescan` gives the same result as without.
- [ ] **Step 2: Run; they fail.**
- [ ] **Step 3: Implement** §4.1, §4.2 and the worker.
- [ ] **Step 4: Run; green.** `/lint`, commit `feat(wreduce): mux and FF rules, worker, caps (3G WR-4/7/10)`, push.

## Task 3: Evidence and the script (WR-9, spec §12 table, the plan's checks)

**Files:**
- Create `tests/opt/rules/wreduce/<rule>.v` + `.il` (≈ 30: one per row of §4 and the pre-step, plus `cascade.v` and `sx_add.v` copied from `exit-repros/sx_add/`), rows in `tests/opt/rules/rules.txt` with the Yosys log needle per row (Yosys's `wreduce` has no `cover` points, so the Yosys side is checked by its `Removed top …` / `Converting cell …` log lines and the cell histogram).
- Create `tools/opt-check/phase3.script` (§9's opt lines).
- Modify `tools/opt-check/opt_rules.py` (a `log:` needle kind beside `cover:`), `tools/opt-check/opt_diff.py` (`--script` accepts `wreduce` and `phase3.script`), their tests in `tests/tools/`, `CMakeLists.txt`, `docs/PASSES.md`, `docs/PHASE3.md` (exit item 3's script).

- [ ] **Step 1: Failing tests:** `tests/tools/test_opt_rules.py` gains the `log:` needle cases (present, absent, count mismatch); CTest `opt_rules` lists the new rows as missing goldens.
- [ ] **Step 2: Per-rule goldens.** `opt-rules --write tests/opt/rules/wreduce` (Odin III `read_verilog -nolatches; proc; opt; wreduce; write_rtlil`; Yosys 0.55 `read_rtlil in.il; wreduce; stat` with the log needle). Every row: counter ≥ 1, Yosys needle present, histograms equal, and `opt-equiv`'s refinement check on `in.il` → `out.il`. The reviewer re-derives five goldens by hand, including `sx_add` and `cascade`.
- [ ] **Step 3: Refinement on the corpus (`opt-equiv`).** `opt-equiv --script phase3.script` on every micro (CTest label `opt_equiv`, sweep preset, smallest first), each `wreduce` call snapshotted and checked.
- [ ] **Step 4: Differential (`opt-diff`).** Same input to both: `S = wreduce` and `S = phase3.script` on the micros (CTest) and on the 19 (heavy, controller, `tools/heavy-run`, smallest first, `mcml` and `LU32PEEng` last). Gate: per-module histograms equal (with A/B/Y widths and flags of every `$add $sub $mul` compared as a multiset), refinement of Yosys's result, identity after the common lowering. A difference fails unless `tests/opt/diff_expect.txt` lists it with a reason; an order-caused one (WR-7) is a finding to fix, not a list entry.
- [ ] **Step 5: The 2H cross-check.** On `stereovision0`, the number of signed `$add` after the Phase 3 opt lines equals the Yosys-own-front-end count (170 of 244 in the 3C survey) or the difference is explained by front-end cell order (WR-7) in the PR text.
- [ ] **Step 6:** `/run-micro` (no regression), `/lint`, commit `test(wreduce): per-rule goldens, refinement, differential (3G §10)`, push, open the PR with the sweep results quoted.

## Self-review

- Spec coverage: WR-1..WR-12 map to Tasks 1 (WR-3.1, 4.3, 5, 6), 2 (WR-3.2, 4.1, 4.2, 7, 10, 11) and 3 (WR-9, 12, 13 and the evidence). The `ABITS 0` IR question (spec §13) waits for Peter and is not implemented.
- Sweeps the controller runs: Task 3 Steps 3 (micros) and 4 (the 19), under the heavy lock.
