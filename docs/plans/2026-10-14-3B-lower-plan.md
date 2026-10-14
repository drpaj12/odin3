# 3B `lower` — Implementation Plan (v1, approved by Peter 2026-10-14, PHASE3 #32)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

v1 (2026-10-14), approved by Peter 2026-10-14 (PHASE3 #32), with
`docs/specs/2026-10-14-3B-lower-design.md` (the spec; its LW-n and § numbers are binding here).

**Goal:** a pass `lower` that turns every word cell of every module into bit cells (`$_AND_`,
`$_OR_`, `$_XOR_`, `$_XNOR_`, `$_NOT_`, `$_MUX_`, `$_DFF_P_`/`_N_`, `$_DLATCH_P_`/`_N_`, constants),
in Yosys 0.55's `simplemap` and `techmap.v` structures, with the word flip-flop family reduced to
one `$_DFF_P_` per bit plus logic in front of D (asynchronous controls made synchronous: decision
19), `$mem` to flip-flops and multiplexers, and hard cells, black boxes, instances, `$sop` and
`$lut` left alone, so that the netlist view (IR-9) holds afterwards.

**Architecture:** `src/passes/lower/` — a driver that visits modules and nodes in ID order and
dispatches by built-in cell type ID to a **builder** (a straight-line function over vectors of net
IDs that appends bit nodes on fresh nets); a per-cell transaction (build, then commit: delete the
word node and merge each output net with the builder's net); a size estimate checked against caps
and the shared byte budget before anything is built; one provenance operation per cell. Evidence in
`tools/lower-check/` (`lower-diff`, `lower-sim`, `lower-equiv`) and `tests/lower/`.

**Tech stack:** C17, `src/util`, `src/ir` (no change), `src/passes/budget.[ch]`, `src/passes/ffinit.[ch]`
(read side), `src/sim` (as an oracle, not a dependency of the pass); Unity; libFuzzer; Python 3
stdlib; Yosys 0.55 (`ODIN3_YOSYS055`, `$VTR_ROOT/build/bin/yosys`, the Parmys plugin's `techlibs/`).

**Spec:** `docs/specs/2026-10-14-3B-lower-design.md` v1.

Also: `docs/IR.md` (IR-1, IR-2, IR-4, IR-6, IR-8, IR-9, IR-12, IR-13, IR-15, IR-16, IR-18, IR-18a),
`docs/DESIGN.md` §5.2, §5.4, §6, §12, PHASE3 (3B row; decisions 3, 4, 6, 7, 16, 17, 19, 28, 29;
risks 5, 6, 8), PHASE2 #25, the 2D spec (EL-27, EL-39, EL-40, EL-48), the 2E spec §13, the 2F
spec/plan (the format, `opt-equiv`/`opt-diff`, `kit_connect`, the budget calls), the 3A spec (FL-17,
`flatten-sim`), Yosys 0.55 `passes/techmap/{simplemap,alumacc,maccmap,dffunmap}.cc`,
`share/techmap.v`, `kernel/ff.cc`, `passes/memory/memory_map.cc`, Parmys's `techlibs/*2dff.v`, and
the standing rules (heavy-run, no nightly, lint).

**Ordering and bases.** `lower` needs only what is on `main`: Phase 1 (registry, 1E, `equiv-check`),
2D (the `$mem` form, IR change A cells), 2E (IR change D cells, `ffinit`, `budget`) and 2F (`opt`,
for the post-`lower` cleanup and as the tools' pattern). It does **not** need 3A (it works per
module) or 3C (a soft `$mul`/`$mem` is just lowered). 3C's soft fallback and the M2 run need 3B
merged. Three PRs, serial:

| PR | Branch | Base | Tasks | Waits for |
|---|---|---|---|---|
| 3B-1 core | `feat/3B-lower-core` | `main` | 1 → 2 → 3 | Peter's approval of spec and plan |
| 3B-2 arithmetic | `feat/3B-lower-arith` | `main` after 3B-1 | 4 → 5 | 3B-1 |
| 3B-3 sequential, memory, evidence | `feat/3B-lower-seq` | `main` after 3B-2 | 6 → 7 → 8 → 9 | 3B-2; Task 9's PHASE3 script edits need Peter's answer to spec §18 question 2 |

**Model/effort** (PHASE0 §9; PHASE3 row 3B: Opus `xhigh` → `high`):

| Task | Implementer | Reviewer |
|---|---|---|
| 1 Scaffold, kit, driver, transaction, registration | Opus `xhigh` | Opus `high` |
| 2 Evidence tools: `lower-diff`, `lower-sim`, fixtures | Opus `high` (fixtures: Sonnet `high`) | Opus `high` |
| 3 Gates and comparisons (§4) | Opus `high` | Opus `high` |
| 4 `$pmux`, shifts, `$alu`, `$neg`/`$add`/`$sub`, comparisons (§5.1–5.4) | Opus `xhigh` | Opus `high` |
| 5 `$mul`, `$div`/`$mod`, `$pow` error (§5.5–5.7) | Opus `xhigh` | Opus `high` |
| 6 Flip-flops, decision 19 (§6.1) | Opus `xhigh` | Opus `high` (Peter reviews the semantics) |
| 7 Latches (§6.2) | Opus `high` | Opus `high` |
| 8 `$mem` (§7) | Opus `xhigh` | Opus `high` |
| 9 `lower-equiv`, fuzz, corpus, determinism, bench, docs | Opus `high` (sweep runs: Sonnet `high`) | Opus `high` |
| Whole-branch review before each PR | — | read-only critique agent (CLAUDE.md rule 12) |

Effort cues: `/model opus` + `/effort xhigh` for Tasks 1, 4, 5, 6, 8; `/effort high` for 2, 3, 7, 9;
`/model sonnet` + `/effort high` for fixture bulk and the Task 9 runs; `/clear` between tasks.

**Lanes.** One lane (this one; lane 2 after 3A). The tasks are serial where they share
`lower.c`/`register.c`/the dispatch table; Task 2 (tools) can start once Task 1's pass exists.
Heavy runs (Task 9) go through `tools/heavy-run` in the controller's announced window (decision 29,
ruling 8; memory `heavy-lock-blocks-builds`); everything else is unit work.

**Pre-flight conflict note.**
1. `src/passes/builtin.c`/`builtin.h`: one table entry (`&odin3_pass_lower`), beside 3A's
   (`flatten`) and 3C's (`hardmap`); append-only, resolved by the later PR.
2. `docs/PASSES.md`: one row (Task 1); the golden-test column completed in Task 9.
   `docs/PHASE3.md`: only the checklist line and a decisions entry (Task 9); 3A/3C append theirs.
3. `CMakeLists.txt`: append-only hunks (unit tests, `lower_cells`, `lower_cells_yosys`,
   `lower_micro`, `cli_lower_*`, the `tests/tools` Python tests, the sweep tests via
   `odin3_add_sweep_test`).
4. `tools/verilog-gen`: Task 9 adds `--lower`, beside the other lanes' generators (append-only).
5. `tests/unit/proc_build.[ch]` (hand-building IR from RTLIL) is used, not changed. Fixtures that
   need cells `proc_build` cannot make use a small shared helper `tests/unit/lower_fixture.[ch]`
   created in Task 1.
6. `src/passes/opt/*` is not touched. `lower_connect` mirrors `kit_connect` (spec §16 row 22).

**Spec points made exact here** (agent defaults, reversible; Task 9 logs them in PHASE3):
- (a) **Entry point:** `odin3_status odin3_lower_run(odin3_pass_ctx *ctx, odin3_design *design,
  const odin3_lower_opts *opts)`, declared in `src/passes/lower/lower.h` (spec §13).
- (b) **A builder** has the signature `odin3_status build_X(lw_ctx *c, const lw_cell *cell,
  lw_bits *out)` where `lw_cell` is a read-only view (type, parameters, port net vectors) and `lw_bits`
  is the vector of result nets, one per output bit; a builder appends to `c->created` (the
  transaction's node list) and calls only `lw_gate_*`, never `odin3_node_create` directly.
- (c) **Gate helpers** `lw_and/or/xor/xnor/not/mux(c, a, b[, s])` return a fresh net; constants
  come from `lw_const(c, 0|1|x)` (the canonical constant net, IR-4). All helpers fail with
  `ODIN3_ERR_NO_MEMORY` and are the only allocation points of a builder, so the OOM test hooks them.
- (d) **Size model:** `size_t lw_estimate(const lw_cell *cell)` in `size.c`, one formula per rule,
  returning an upper bound on nodes; the unit tests assert `created ≤ estimate` and print the ratio.
- (e) **Dispatch** is a static table `{builtin_index, build_fn, estimate_fn, flags}` sorted by
  built-in index, checked for completeness against the registry by a unit test: every `word` type in
  the registry has an entry or is in the explicit `no_rule` list (`$proc`, `$tribuf`, `$pow` →
  error text, `$memrd`, `$memwr`), so a cell type added later fails the test, not a design.
- (f) **Provenance:** one operation per word cell, begun before the first created node and ended
  after the commit.
- (g) **Tools** live in `tools/lower-check/` (Python 3 stdlib, `ruff`/`mypy --strict` clean, tests
  under `tests/tools/`), with the 2F tools' conventions: exit 0 / 1 / 2 / 3 (3 = hard: crash, cap
  kill, timeout), every child through `memcap.run`.

---

## Task 1 — Scaffold, kit, driver, transaction, registration (PR 3B-1)

**Files.**
- Create `src/passes/lower/{lower.h,lower_test.h,register.c,lower.c,kit.c,connect.c,size.c,dispatch.c}`.
- Create `tests/unit/test_lower_kit.c`, `tests/unit/test_lower_pass.c`, `tests/unit/lower_fixture.[ch]`.
- Modify `src/passes/builtin.[ch]`, `docs/PASSES.md` (a `lower` row), `CMakeLists.txt`.

**Interfaces.**
- `odin3_lower_opts`, `odin3_lower_run` (spec §13; plan (a)).
- `lw_ctx` (module, design, ctx, provenance op, `created` vec, options, caps, counters),
  `lw_bits` (a `src/util` vec of net IDs), `lw_extend(c, bits, width, signed, out)` (spec §3.3),
  `lw_gate_*`, `lw_const`, `lw_tree(c, kind, bits, out)` (the pairing tree of spec §4),
  `lw_commit(c, cell, outs)` and `lw_rollback(c)` (spec §12.2), `lw_connect(c, keep, drop)`
  (spec §3.2).
- The pass entry: `lower [--neg-edge invert|keep] [--async sync|error] [--report <file>]
  [--max-mem-bits N]`, `processes = REFUSE`; for now the dispatch table is empty, so every word
  cell is `lower: no lowering rule for cell type …`.

- [ ] **Step 1: Tests first.** `test_lower_kit.c`: extension (signed, unsigned, truncate, both-signed
  and mixed flags), the pairing tree for 1..40 inputs against the closed form (nodes, depth, odd
  carry), `lw_connect` (alias and name kept, a port net, the canonical constant, `keep == drop`),
  rollback after a failure at each allocation, OOM at every allocation of every helper.
  `test_lower_pass.c`: option parsing and unknown arguments, the `REFUSE` refusal with a `$proc` in a
  module, an empty design (`lower: nothing to lower`), a `read_blif` netlist unchanged byte for byte,
  the dispatch completeness test of plan (e).
- [ ] **Step 2: Driver.** Modules in module-ID order, nodes in ID order; granularity switch (spec
  §3.1); per-cell transaction; provenance op; the INFO/DEBUG lines; the post-condition of spec §12.3
  (Debug assert; returns `ODIN3_ERR_CHECK` in Release only if cheap, else assert).
- [ ] **Step 3: Measure `odin3_net_merge`.** Write a test that injects an allocation failure inside
  `odin3_net_merge` on a net with an alias and a port. Record in the task note whether it can fail.
  This decides spec §12.2's commit contract (full atomicity or "partial but valid"). Update spec
  §12.2 in the PR if the answer is "cannot fail".
- [ ] **Step 4: Caps and budget.** The three caps of spec §11 as named constants, lowered through
  `lower_test.h`; `lw_estimate` stub returns the exact bound for the rules that exist; the
  `odin3_budget_check` call every 2^16 created nodes.
- [ ] **Step 5: Registration.** Pass table entry; `docs/PASSES.md` row (view: RTLIL view or mixed →
  netlist view; `REFUSE`); `stats`-based test that the pass logs its counts.
- [ ] **Step 6: Verify.** `cmake --preset debug && cmake --build --preset debug`; `ctest --preset
  debug -R lower` green under ASan/UBSan; `tools/lint.sh` green (the full gate in CI). Quote the
  commands and their output in the commit message body.
- [ ] **Step 7: Commit and push** (CLAUDE.md rule 8): `feat(3B): lower pass scaffold, kit, driver`.

## Task 2 — Evidence tools: `lower-diff`, `lower-sim`, fixtures (PR 3B-1)

Written before the rules so every later task is test-first against the oracle.

**Files.**
- Create `tools/lower-check/{lower-diff,lower-sim,lower_diff.py,lower_sim.py,README.md}`,
  `tools/lower-check/maps/{adffe2dff_reset_first.v,aldffe2dff_reset_first.v,dffsre2dff.v}` (the
  declared-reference maps of spec §6.3 and §14.2 tier D, copied from Parmys's `techlibs/` with the
  enable order changed; the originals are read-only under `external/`).
- Create `tests/tools/test_lower_diff.py`, `tests/tools/test_lower_sim.py`.
- Create `tests/lower/README.md` and the directory layout `tests/lower/{cells,hand}/`.
- Modify `CMakeLists.txt` (the Python tests; `lower_cells`, `lower_cells_yosys` shells).

**Interfaces.**
- `lower-diff <files> [--top T] [--family F]`: spec §14.2 steps 1–3; exit 0 all cells agree to their
  tier, 1 a difference, 2 tool error, 3 hard. Prints per-cell-type histograms of both sides.
- `lower-sim <files> [--top T] [--vectors N] [--seed S]`: spec §14.3; prints the sample points and,
  per cell type, `pass`/`skipped (reason)`, never `pass` for a skip.
- Both need Yosys 0.55 only for `lower-diff`; `lower-sim` needs `odin3` and, for FF designs, the
  independent reference stepper (Task 6 supplies it; until then FF designs are `skipped`).

- [ ] **Step 1: Tests first** (`unittest`): `lower-diff` on a pair of known-equal and known-different
  BLIFs (mutant: swap two gate inputs; exit 1); `lower-sim` on a hand-built design (a `$and` and a
  `$mux`) with an injected mutant of the lowered netlist (exit 1) and on a `$shiftx` (reported
  `skipped`, not `pass`).
- [ ] **Step 2: `lower-diff`.** Odin III side: the script of spec §14.2 step 1 through `odin3`;
  Yosys side: step 2 through `memcap.run` under `prlimit --as=3 GiB`; a Yosys other than 0.55 is
  refused (as the 2F tools do); compare per tier with `netlist-compare` or `equiv-check`;
  `diff_expect.txt` handling.
- [ ] **Step 3: `lower-sim`.** Build the before/after pair, run 1E `sim` on both with the same
  vectors, compare per cycle; the skip table of spec §14.3.
- [ ] **Step 4: Fixture helpers** `tests/unit/lower_fixture.[ch]`: build a one-cell module of a given
  type, widths, signs and parameters with primary inputs/outputs, so the unit tests of Tasks 3–8 and
  `lower-sim` share them; plus `tools/lower-check/gen_cells.py` that writes `tests/lower/cells/*.v`
  for a (type, width, sign) grid.
- [ ] **Step 5: Verify.** `ctest --preset debug -R 'lower|tools'`; `ruff check` and `mypy --strict`
  on the new Python; run both tools on a hand-built `$and` design by hand and quote the output.
- [ ] **Step 6: Commit and push.**

## Task 3 — Gates and comparisons: the `simplemap` family (PR 3B-1)

**Files.** Create `src/passes/lower/bitwise.c`, `tests/unit/test_lower_bitwise.c`,
`tests/lower/cells/{not,and,or,xor,xnor,mux,reduce_*,logic_*,eq,ne}_*.{v,il,blif}`. Modify
`dispatch.c`, `size.c`, `CMakeLists.txt`.

**Interfaces.** `build_not`, `build_bitop` (and/or/xor/xnor), `build_mux`, `build_reduce` (and,
or, xor, xnor, bool), `build_logic_not`, `build_logic_bin` (and/or), `build_eqne` (spec §4).

- [ ] **Step 1: Tests first** (`test_lower_bitwise.c`): for each type, widths {1, 2, 3, 4, 5, 8, 17,
  33, 64, 65}, unequal operand widths, signed/unsigned/mixed (the mixed warning text pinned):
  node counts against closed forms (`$and` of width W: W nodes; reduction of width W: W−1 (+1 for
  xnor); `$eq` of width W: W XOR + (W−1) OR (+1 NOT)); the exact tree shape of a 5-input reduction
  (spec §4: carried odd bit); `Y[k>0]` is the constant-0 net; a 1-bit reduction creates no node;
  the netlist view; names and wire membership of `Y` unchanged; **sim-equal** (1E registry hook vs
  lowered), exhaustive for total input width ≤ 12, 4,096 seeded random vectors above.
- [ ] **Step 2: Implement** the nine builders (each ≤ 60 lines), `lw_tree` shared by all reductions.
- [ ] **Step 3: Goldens.** `gen_cells.py` writes the grid; commit the `.blif` produced by the pass
  (after review against Yosys, step 4); CTest `lower_cells` compares with `netlist-compare`.
- [ ] **Step 4: Against Yosys.** `lower-diff --family bitwise` over the grid: **tier S, exit 0**.
  Any difference is either a bug here or a listed divergence in the spec (update §16 if so).
- [ ] **Step 5: Verify.** `ctest --preset debug -R 'lower'`; `lower-diff` and `lower-sim` output
  quoted; `tools/lint.sh`.
- [ ] **Step 6: Commit and push; open PR 3B-1** after the whole-branch critique review (rule 12),
  listing rejected findings in the description. Merge when `ci` is green.

## Task 4 — `$pmux`, shifts, `$alu`, `$neg`/`$add`/`$sub`, comparisons (PR 3B-2)

**Files.** Create `src/passes/lower/{pmux_shift.c,alu.c}`, `tests/unit/test_lower_pmux_shift.c`,
`tests/unit/test_lower_alu.c`, `tests/lower/cells/{pmux,shl,shr,sshr,shiftx,neg,add,sub,lt,le,gt,ge}_*`.
Modify `dispatch.c`, `size.c`.

**Interfaces.** `build_pmux`, `build_shift` (`$shl $shr $sshr`), `build_shiftx`, `lw_alu(c, a, b,
ci, bi, ywidth, signed, need_sum, out)` and `lw_lcu(c, p, g, ci, out)` (internal, tested alone),
`build_addsub`, `build_neg`, `build_cmp` (spec §5.1–5.4).

- [ ] **Step 1: Tests first.** `lw_lcu` against a software carry-lookahead model for W = 1..70
  (including the powers of two and their neighbours, where Brent-Kung's loops change); `lw_alu`
  sum and carries against integer arithmetic; `$add`/`$sub`/`$neg` at the widths of Task 3, with
  `Y_WIDTH` smaller than, equal to and larger than the operands, signed/unsigned/mixed; the four
  comparisons, signed and unsigned, with `CO[W−2]` = `CI` at W = 1; shifts with `B` narrower than,
  equal to and wider than `BB_WIDTH`, all three types, signed `A`; `$shiftx` including a signed
  negative `B` and an out-of-range `B` (x outputs). Node-count tables measured and pinned (the
  constants for `size.c`); `created ≤ estimate`. Sim-equal for everything with a hook; `$shiftx`
  `skipped`. A `$pmux` with one-hot selects, with the multi-hot case checked as the OR of words.
- [ ] **Step 2: Implement** `lw_lcu` (explicit loops, no recursion), `lw_alu` (lazy sum bits for
  comparisons), `build_pmux`, `build_shift`, `build_shiftx`, `build_addsub`, `build_neg`,
  `build_cmp`. Read `techmap.v` lines 76–190, 209–290 and `alumacc.cc` lines 44–110 again while
  writing; each builder cites its Yosys source lines in a comment.
- [ ] **Step 3: Goldens and Yosys.** `gen_cells.py` grid; `lower-diff --family arith,shift,pmux`:
  **tier T**, `equiv-check` exit 0 and histograms printed; write `tests/lower/diff_expect.txt`
  entries only for differences with a reason. Record in the task note the fraction of cells whose
  histogram is identical after `opt -fast`.
- [ ] **Step 4: `lw_estimate`** formulas for these rules; calibrate constants from step 1.
- [ ] **Step 5: Verify.** `ctest --preset debug -R lower`; tools output quoted; `tools/lint.sh`.
- [ ] **Step 6: Commit and push.**

## Task 5 — `$mul`, `$div`/`$mod`, `$pow` (PR 3B-2)

**Files.** Create `src/passes/lower/{mul.c,div.c}`, `tests/unit/test_lower_mul.c`,
`tests/unit/test_lower_div.c`, `tests/lower/cells/{mul,div,mod,pow}_*`. Modify `dispatch.c`, `size.c`.

**Interfaces.** `lw_heap` (the bit heap: columns of nets, add with cancel-and-carry), `lw_fa(c, a, b,
cin, sum, carry)`, `build_mul`, `lw_divmod_u`, `build_div`, `build_mod`, `build_pow` (always the
error of spec §5.7).

- [ ] **Step 1: Read `maccmap.cc` (lines 30–260) line by line** and write down in the task note the
  exact order in which summands are formed and compressed; the spec §5.5 summary is binding for the
  contract (counts of `$_AND_` and full adders, then the histogram), and the order is binding for
  tier T identity.
- [ ] **Step 2: Tests first.** `$mul` at W = 1..16 exhaustively for the operand pairs (signed and
  unsigned; `Y_WIDTH` = W, 2W, and smaller), then random up to 64; `$div`/`$mod` likewise (including
  divisor 0: quotient all ones, refining x; the most negative number divided by −1); `lw_heap`
  cancel-and-carry; `lw_fa` truth table; node counts pinned; `$pow`: the exact error text, and
  that nothing is built; the cap: a `$div` of width 3,000 is the located cap error, nothing built,
  design intact.
- [ ] **Step 3: Implement** `mul.c` (heap, FA tree, final `lw_alu`), `div.c` (absolute values,
  restoring stages, sign fix-up), `build_pow`.
- [ ] **Step 4: Goldens and Yosys.** `lower-diff --family mul,div`: tier T. Multipliers above 16 bits
  and dividers above 24 bits are compared by `equiv-check` only if it finishes inside its 60 s limit,
  else `unproven` (listed, not passed).
- [ ] **Step 5: `lw_estimate`** for both; the per-cell cap test.
- [ ] **Step 6: Verify, commit, push; open PR 3B-2** after the critique review.

## Task 6 — Flip-flops and decision 19 (PR 3B-3)

**Files.** Create `src/passes/lower/ff.c`, `src/passes/lower/report.c`,
`tests/unit/test_lower_ff.c`, `tests/unit/lower_ffref.[ch]` (the independent reference stepper),
`tests/lower/cells/{dff,dffe,sdff,sdffe,sdffce,adff,adffe,aldff,aldffe,dffsr,dffsre}_*`. Modify
`dispatch.c`, `register.c` (`--neg-edge`, `--async`, `--report`), `size.c`; `tools/lower-check` (the
stepper hook in `lower-sim`).

**Interfaces.** `build_ff(c, cell, outs)` for the eleven types; `lw_next_state(c, cell, bit, out)`
(spec §6.1's table, one function per family shape); `lw_storage(c, clk, d, q, init, neg)` (creates
`$_DFF_P_`, or `$_NOT_` + `$_DFF_P_`, or `$_DFF_N_`); `lw_report_add(...)` and the JSON writer
(spec §10).

- [ ] **Step 1: Independent reference stepper first.** `lower_ffref.c`: the simlib semantics of the
  eleven types as C (`ff.c`'s comment table), no code shared with the pass, one `step(state, inputs)`
  per type. Its own test checks it against hand-computed sequences. (This is the "before" side of
  spec §14.3 for FFs.)
- [ ] **Step 2: Tests first** (`test_lower_ff.c`): every type × polarity of every control ×
  `CLK_POLARITY` × width {1, 4, 33}: the structure of spec §6.1's table (node types and counts), the
  netlist view, init propagation (0, 1, x → 3) from the `init` wire attribute, `--neg-edge` both ways
  (one `$_NOT_` per cell, shared), `--async error` (the exact text), the once-per-design warning with
  exact counts, the `--report` JSON; the **behavioural check**: 1E on the lowered netlist against
  `lower_ffref` over all input sequences of length ≤ 5 at W = 1 and random sequences at larger
  widths, sampling at the end of each cycle (spec §14.3). The `$adffe`/`$aldffe`/`$dffsre` cases
  are run in both enable orders (the hidden `--async-enable-first`) and each is checked against its
  own reference variant.
- [ ] **Step 3: Implement** `lw_next_state` (polarity folded by swapping mux arms; no inverter
  nodes), `lw_storage`, `build_ff`, the report.
- [ ] **Step 4: Against Yosys and the declared reference.** `lower-diff --family ff`: tier S for
  `$dff $dffe $sdff $sdffe $sdffce` (Yosys `dffunmap`, **netlist-compare exit 0**); tier D for the
  async families against the Yosys lowering with `tools/lower-check/maps/*.v` (`equiv-check` exit 0).
  Also run the *unmodified* Parmys maps and confirm they differ from ours exactly on `$adffe`,
  `$aldffe` with the enable low and the reset high (a test that documents divergence #4 with a
  counter-example sequence).
- [ ] **Step 5: Verify.** `ctest --preset debug -R lower`; `lower-diff`, `lower-sim` quoted;
  `tools/lint.sh`.
- [ ] **Step 6: Commit and push.** Peter reviews this task's semantics in the PR (decision 19).

## Task 7 — Latches (PR 3B-3)

**Files.** Create `src/passes/lower/latch.c`, `tests/unit/test_lower_latch.c`,
`tests/lower/cells/{dlatch,adlatch,dlatchsr,sr}_*`. Modify `dispatch.c`, `size.c`.

**Interfaces.** `build_dlatch`, `build_adlatch`, `build_dlatchsr`, `build_sr` (spec §6.2): enable
`E'` and data `D'` per bit, then `$_DLATCH_P_` (`$_DLATCH_N_` never needed: polarity is folded into
`E'`... except `$dlatch` with `EN_POLARITY = 0`, which uses `$_DLATCH_N_` directly).

- [ ] **Step 1: Tests first.** Behavioural: 1E on the lowered netlist against reference formulas
  (level semantics; transparent while `E'` is high; clear wins over set) for all input sequences of
  length ≤ 4 at W = 1; structure counts; `INIT`; the latch enable from logic is accepted by `check`.
- [ ] **Step 2: Implement** the four builders.
- [ ] **Step 3: Against Yosys.** `lower-diff --family latch`: tier S for `$dlatch`; tier D for the
  others against Yosys `dfflegalize -cell $_DLATCH_?_ 01` (`equiv-check`).
- [ ] **Step 4: Verify, commit, push.**

## Task 8 — `$mem` to flip-flops and multiplexers (PR 3B-3)

**Files.** Create `src/passes/lower/mem.c`, `tests/unit/test_lower_mem.c`,
`tests/lower/hand/mem_*.{il,blif}`. Modify `dispatch.c`, `size.c`, `register.c`
(`--max-mem-bits`).

**Interfaces.** `build_mem(c, cell, outs)` (spec §7): storage FFs, `lw_addr_decode`, the write mux
chain in port order, the read trees (async) and read registers (sync, not transparent), the errors
and the cap; `lw_estimate` = `O(SIZE·WIDTH·(WR_PORTS + 1) + RD_PORTS·SIZE·WIDTH)` with measured
constants.

- [ ] **Step 1: Read `memory_map.cc` (lines 100–420) and `Mem::extract_rdff` in `kernel/mem.cc`**;
  note in the task whether a transparent read can be added cheaply (spec §7 makes it an error; the
  note is for Peter's later review, not a change).
- [ ] **Step 2: Tests first.** A C array model drives the lowered netlist through 1E: write all
  addresses then read all, with one and two write ports (later wins), `OFFSET` 0, 1 and −2,
  `ABITS` smaller and larger than `clog2(SIZE)` (out-of-range reads give x, writes ignored), byte
  enables (`WR_EN` bits differing), `RD_EN` low on a sync port, `INIT` contents (read before any
  write), a `$mem` with no read port (deleted), a write port with constant-0 `WR_EN` (skipped);
  every error text (async write, mixed clocks, transparent read, standalone `$memrd`/`$memwr`, the
  cap); the INFO/WARN lines; `created ≤ estimate`.
- [ ] **Step 3: Implement** `build_mem`.
- [ ] **Step 4: Against Yosys.** `lower-diff --family mem`: tier D against `memory -nomap;
  memory_map` (`equiv-check`, small memories only: SIZE ≤ 16, WIDTH ≤ 4; larger are `unproven`
  and listed). RAM has no 1E oracle for designs from the front end (PHASE3 risk 5); this and the C
  model are the evidence.
- [ ] **Step 5: Verify, commit, push.**

## Task 9 — Refinement checks, corpus, fuzz, determinism, bench, docs (PR 3B-3)

**Files.**
- Create `tools/lower-check/{lower-equiv,lower_equiv.py}`, `tests/tools/test_lower_equiv.py`,
  `tests/fuzz/fuzz_lower.c`, `tests/bench/bench_lower.c`, `tests/lower/{diff_expect.txt,vtr_expect.txt}`,
  `tests/cli/lower_*.sh` (or the repo's CLI test form).
- Create `src/passes/lower/snapshot.c` (`--lower-snapshot`, Debug-only).
- Modify `tools/verilog-gen` (`--lower` weighting), `docs/PASSES.md` (golden-test column),
  `docs/PHASE3.md` (checklist line "3B `lower`"; a decisions entry with the agent defaults; the exit
  script text if Peter answers question 2 yes), `docs/DESIGN.md` §6 step 9 (one sentence), the 3H
  spec and plan script strings (same condition), `CMakeLists.txt`.

**Interfaces.**
- `lower-equiv <files> [--top T]`: spec §14.4; exit 0 refines, 1 not, 2 tool error, 3 hard.
- `lower --lower-snapshot <dir>` (Debug): `write_rtlil` before and `write_blif` after, per module.
- `fuzz_lower`: libFuzzer harness; `bench_lower [--cells N]`.

- [ ] **Step 1: Tests first** (`unittest`): `lower-equiv` on correct lowerings (exit 0) and on the
  regression mutants of spec §14.4 (each exit 1): flipped edge, wrong clock net, wrong function,
  wrong reset value/polarity, tied output, missing port, reset and enable swapped, bit-swapped
  `$pmux`, signed comparison read unsigned, `$shr` shifted by one, wrong `$mem` `OFFSET`; a hard
  failure (a crashing child) is exit 3 and never absorbed.
- [ ] **Step 2: `lower-equiv` and `--lower-snapshot`**, with 2F's clock-aware lowering family
  (`tools/opt-check/clock_sig.py` is imported, not copied).
- [ ] **Step 3: Script test (O2).** The Phase 3 script on a hand-built design with a `multiply`, an
  `adder`, a `single_port_ram` and a soft `$mul`, `$add`, `$mem`: the hard cells survive; the soft
  ones go; counts logged; `check` clean; plus the same through the CLI.
- [ ] **Step 4: Corpus.** `lower_micro` over `tests/micro/verilog` (script: `read_verilog
  --nolatches; proc; opt; lower; opt -fast; compact`), smallest first, `ODIN3_OK` and the netlist view;
  `lower-sim` and `lower-equiv` over the same (RAM micros: `lower-equiv` only). `lower_vtr` over the
  19 VTR designs smallest first through `tools/heavy-run`, caps lowered per decision 17; before 3C
  lands, record the `cap` outcomes in `vtr_expect.txt` (spec §14.5); after 3C the file must be empty.
- [ ] **Step 5: Fuzz and generator.** `fuzz_lower.c` (bytes → word-cell builder → `lower` with
  lowered caps and injected `NO_MEMORY` → `check` FULL), a 60-second CTest run; `tools/verilog-gen
  --lower`: 200 seeds through `lower-diff` and `lower-equiv` in CTest when Yosys 0.55 is found; the
  5,000-seed run in a heavy-run window.
- [ ] **Step 6: Determinism, OOM, provenance, bench.** Two runs byte-identical (`write_rtlil`,
  `write_blif`); module built in reverse order → `netlist-compare` identical; OOM at every allocation
  of a fixture per rule family (spec §12.2's two contracts); provenance reaches the dead cell;
  `bench_lower` Release number into the PHASE3 entry (`--cells 1000000`, peak RSS under the 60%
  budget).
- [ ] **Step 7: Docs.** The PHASE3 checklist line ticked with the evidence commands quoted; the
  decisions entry (agent defaults of spec §18's last paragraph and Peter's answers); DESIGN §6 step 9
  sentence ("`lower` is Yosys 0.55's `simplemap`/`techmap.v` structures; asynchronous controls become
  synchronous, decision 19"); the script text edits (conditional on question 2).
- [ ] **Step 8: Whole-branch critique review** (rule 12); triage; one re-check per high finding.
- [ ] **Step 9: Verify.** `ctest --preset debug` (all), `ctest --preset release -R 'bench|lower'`,
  `tools/lint.sh` (full, with clang-tidy), the heavy-run window's output quoted. Open PR 3B-3.

---

## Done when

- Every rule of spec §4–§8 has a unit test, a golden and a `lower-diff` verdict at its tier.
- `lower-sim` is `pass` for every combinational cell and every micro without RAM, `pass` against the
  reference for the FF/latch families, and nothing is `pass` by skipping.
- `lower-equiv` is exit 0 on the corpus (unproven designs listed).
- The netlist view holds after the pass on every corpus run; two runs are byte-identical.
- `docs/PASSES.md` has its row; the PHASE3 checklist line is ticked with the evidence; Peter has
  answered spec §18.
- 3C can run its soft fallback through `lower`, and M2's `lower_vtr` list (`vtr_expect.txt`) is
  empty once 3C is merged.
