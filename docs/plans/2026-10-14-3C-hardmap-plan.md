# 3C `hardmap` — Implementation Plan (v1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

v1 (2026-10-14). The spec `docs/specs/2026-10-14-3C-hardmap-design.md` was approved by Peter on
2026-10-14 ("All 10 yes", PHASE3 #31); its rule IDs (M-n, A-n, R-n, S-n, H-n) and D-numbers are binding
here. The plan itself follows the standing rule of PHASE3 #32: choices the spec leaves open are agent
defaults, listed under "Spec points made exact" and logged in PHASE3 by Task 7. The only item that may
need Peter is under "Needs Peter".

**Goal:** a pass `hardmap` that binds word cells to the VTR arch's hard blocks exactly where Parmys's
defaults do: `$mul` to one `multiply` (WIDTH 36), `$add`/`$sub` to `adder` chains, RAM instances and
inferred `$mem` to 1-bit `single_port_ram`/`dual_port_ram` slices. Signed operands are sign-extended (D1,
D3), the depth-split select is registered (D4), and every other difference from Parmys is one of the
declared D-numbers.

**Architecture:** `src/passes/hardmap/`. A driver visits modules and cells in node-ID order (`$mul`, then
`$add`/`$sub`, then RAM instances and `$mem`, H-8). Each source cell is one transaction: build every new
node and net on fresh nets, then reserve, delete the source and merge its output nets into the fresh ones
(the order of `odin3_kit_connect`, so `NO_MEMORY` leaves the source intact, H-9). One provenance operation
and one DERIVED record per source cell, parent = the source's record (H-7). A small local kit
(`hm_kit.[ch]`) creates cells, names, attributes and glue; it does not depend on `opt_ctx` (as 3B's
`lower_connect`). Evidence: Parmys probes first (Task 1), then unit tests with before/after simulation
(1E) per rule, then `tools/hardmap-check/hardmap-diff` against the Parmys goldens.

**Tech stack:** C17, `src/util`, `src/ir` (no change), `src/passes/budget.[ch]`, `odin3_opt_both_signed`
(`src/passes/opt/kit.h`, a pure function), `src/sim` as oracle only; Unity; Python 3 stdlib; iverilog and
Verilator (probes); Yosys 0.55 + Parmys (`$VTR_ROOT`); `tools/equiv-check`, `tools/netlist-compare`,
`tools/heavy-run`.

**Spec:** `docs/specs/2026-10-14-3C-hardmap-design.md` v1. Also `docs/IR.md` (IR-2, IR-7b, IR-8/9, IR-12,
IR-13, IR-15), PHASE3 decisions 3, 4, 5, 6, 16, 19, 29-32, the 3G spec/plan (WR-9 script), the 3B spec
(O2: `lower` never opens hard cells), the 3A spec/plan (`flatten`), the 3E spec (VB-8 hard blocks, 3E-3
`unconn`), the 3H plan (Tasks 11, 14, 15), `lib/vtr.o3lib`, and Parmys's `parmys.cc`, `core/{adder,
subtractor,multiplier,block_memory,memory}.cc` (read only).

**Branches and PRs** (three, serial; the pass is registered in 3C-1 with the `$mul` rule only and gains
rules per PR; `docs/PASSES.md` says so until Task 7):

| PR | Branch | Base | Tasks | Waits for |
|---|---|---|---|---|
| 3C-1 | `feat/3C-hardmap-mul` | `main` | 1 -> 2 | nothing (unit tests run on hand-built flat IR) |
| 3C-2 | `feat/3C-hardmap-add` | `main` after 3C-1 | 3 -> 4 | 3C-1; Task 4's script legs need `lower` (3B) on `main`, else they skip (exit 77) |
| 3C-3 | `feat/3C-hardmap-mem` | `main` after 3C-2 | 5 -> 6 -> 7 | 3C-2; Task 7 needs 3A, 3G, 3B merged and an idle machine |

**Model/effort** (PHASE3 row 3C: Opus `xhigh` -> `high`; token plan: Sonnet for bulk):

| Task | Implementer | Reviewer |
|---|---|---|
| 1 Parmys probes, `parmys-probe` | Sonnet `high` | Opus `high` |
| 2 Scaffold + `$mul` | Opus `xhigh` | Opus `high` |
| 3 Adder chains | Opus `xhigh` | Opus `high` |
| 4 `hardmap-diff`, micros | Opus `high` (fixtures: Sonnet `high`) | Opus `high` |
| 5 RAM instances | Opus `xhigh` | Opus `high` |
| 6 Inferred `$mem` | Opus `xhigh` | Opus `high` |
| 7 The 19, divergence rows, docs | Opus `high` (runs: Sonnet `high`) | Opus `high` |
| Whole-branch review per PR | - | read-only critique agent (CLAUDE.md rule 12) |

Effort cues: `/model opus` + `/effort xhigh` for 2, 3, 5, 6; `/effort high` for 4, 7; `/model sonnet` +
`/effort high` for 1 and the Task 7 runs; `/clear` between tasks.

**Lanes.** One lane (lane 3, after 3G). The tasks are serial (they share `hardmap.c`, `register.c`, the
glue kit). Heavy runs (Task 7) go through `tools/heavy-run`, run by the controller in an announced window
(PHASE3 #29 ruling 8; memory `heavy-lock-blocks-builds`).

## Global Constraints

- **Code standard** (CLAUDE.md): C17, `-Werror`, lizard <= 15 CCN / 60 lines / 5 parameters, no VLA, no
  recursion in `src/` (R-2's depth split is a loop over levels or an explicit stack), only `cleanup:`,
  named constants (36, 15, 72, 2^16 are named), `src/util` containers, no `NOLINT`.
- **Effective Parmys configuration** (spec H-1): `fixed_hard_multiplier` 1 (WIDTH **36**, pad to size);
  `split_hard_multiplier` 0 (never 18x18 or 9x9); `mult_padding` -1 (pad = `$_CONSTX_` net, written as
  `unconn`); `min_threshold_adder` 1; `min_add` 0; `fixed_hard_adder` 1; `adder_cin_global` false (dummy
  head); `split_memory_width` 1 (1-bit slices); `split_memory_depth` **15** (address bits per RAM); soft
  memory thresholds 0.
- **Rule values copied verbatim:** `$mul` goes hard unless `a = b = 2`, or `max(a, b) > 36`, or signed
  with `Y_WIDTH > 36` (M-2, M-5); chain length is `n + 2` with `n = max(A_WIDTH, B_WIDTH)` (A-3); a
  subtraction is `a + ~b + 1` with head `a = b = vcc`, `cin` = pad (A-5); the signedness test is
  `odin3_opt_both_signed` (PHASE2 #25); order is depth split, width split, address padding (R-1).
- **H-3:** the four hard types are found by name (`adder`, `multiply`, `single_port_ram`,
  `dual_port_ram`) and are never created; a missing or non-`hard` type is the located error
  `hardmap: cell type 'multiply' is not registered; read_techlib lib/vtr.o3lib first`.
- **IR discipline:** build before the first destructive step; `NO_MEMORY` leaves the IR valid and the
  source cell intact (H-9); names are never dropped (IR-2); no compaction (PHASE2 #19); the pass is
  `processes = REFUSE`; every test that builds IR runs `odin3_check_design` FULL; `check` runs before
  and after in Debug.
- **Determinism** (H-8): node-ID order; two runs byte-identical (`write_blif`, `write_rtlil`).
- **Diagnostics:** the spec's texts verbatim (H-10, M-6, R-9, R-11, R-12); tests assert whole lines
  through a capturing sink.
- **Budget:** `odin3_budget_check` per cell kind and every 2^16 created pins (H-9).
- **Builds:** `tools/build-locked --preset debug -j6 && ctest --preset debug -j4`. No build while a
  heavy run holds the lock. Sweeps only through `tools/heavy-run`, run by the controller, smallest input
  first, `mcml` and `LU32PEEng` last.
- The Phase 3 script ahead of `lower` (3G WR-9, 3B): `read_techlib lib/vtr.o3lib; read_verilog
  --vtr-compat --nolatches D.v; proc; opt; wreduce; opt_clean; opt -full; flatten; opt -full; hardmap;
  opt -full; lower; opt -fast; compact; write_blif --vtr out.blif` (the exact spelling is whatever 3G/3B
  merged; Task 4 reads it from one file, `tools/hardmap-check/phase3_pre_abc.script`).
- `/lint` (`tools/lint.sh`) before each commit; commit and push at each green checkpoint.

## Review Focus

Failure modes the spec implies but its own rules do not pin; the test is added to the owning task.

1. **Zero-width and degenerate operands** (`$mul` with `A_WIDTH 0`, `$add` with both widths 0, `Y_WIDTH`
   below the chain, `ABITS 0` from 3G). Expected: left alone silently or mapped with only the Y bits that
   exist, never a crash or an out-of-range pin. Tasks 2, 3, 6.
2. **Aliased operand and result bits** (`a + a`, `a - a`, a repeated sign bit, a Y bit that is also a
   module output port, two Y bits merged onto one net by A-7). Expected: correct simulation and a clean
   `check`; if `check` rejects two output ports on one net, stop and ask (Needs Peter, item 1). Task 3.
3. **Idempotence and explicit hard cells** (`hardmap` run twice; a design that already instantiates
   15-bit 1-bit RAMs, 36-wide `multiply`, 1-bit `adder`). Expected: second run changes nothing and logs
   `0 multiply, 0 adder ...`. Tasks 2, 3, 5.
4. **Address widths around the split** (`ADDR_WIDTH` 1, 15, 16, 17, a `$mem` with `OFFSET != 0`, a falling
   clock edge). Expected: 15 maps unsplit, 16 and 17 give 2 and 4 RAMs with 1 and 2 registered selects per
   read port, the last two stay soft with a warning (agent defaults e and f). Tasks 5, 6.
5. **Same-address dual write and read-first semantics** (R-6; an address-merged read port next to a write
   on the same clock). Expected: R-6 adds no logic; a transparent read is never mapped to a read-first
   RAM (agent default d). Tasks 5, 6.

## Spec points made exact here (agent defaults, reversible; Task 7 logs them in PHASE3)

- (a) **Entry point:** `odin3_status odin3_hardmap_run(odin3_pass_ctx *ctx, odin3_design *design)`, in
  `src/passes/hardmap/hardmap.h`; no options. Stats struct `odin3_hardmap_stats { uint64_t multiply,
  adder, chains, sp_slices, dp_slices, soft; }` feeds the H-10 INFO line.
- (b) **Names and attributes** (reverse engineering, D11). A source cell's first created node takes the
  cell's name; the others take `<cell>.<role>`, made unique by the IR's existing `_N` rule. Every created
  hard cell and glue node gets the string attribute `hardmap` = `<kind>:<role>` (`multiply:mul`,
  `adder:head`, `adder:s3`, `adder:top`, `single_port_ram:b5`, `dual_port_ram:b5.h`, `$mux:b5.sel0`).
  The source is reachable through the DERIVED record (IR-12: a DERIVED record resolves to its first
  leaf's source location and `hier`). No IR change.
- (c) **Y bits above the chain** (A-7) alias the result sign net with `odin3_net_merge` (IR-15), as
  3A's feed-throughs do; no buffer cell exists.
- (d) **Address-merge R-8 is applied only when the `$mem` has no write port on the read clock**, pending
  Task 1's probe `addrmerge.v`. The hard RAMs are read-first (`vtr_flow/primitives.v`, "Old data
  read-first"), so a merged read of the registered address would return the old word after a
  same-address write where the RTL returns the new one. If Parmys is RTL-correct there (not expected), the
  spec's R-8 stands unchanged. Otherwise this is divergence D12 (port stays asynchronous, R-9) and the
  spec's `RD_TRANSPARENT = 1` text is not used. Data-merge ports are unaffected. The merge is part of the
  mapping transaction: a `$mem` that ends soft keeps its original FFs.
- (e) **`OFFSET != 0`** (the spec is silent; Parmys records the offset and ignores it, which is wrong):
  the `$mem` stays soft with `hardmap: memory '<m>' has OFFSET <n>; left to lower` (R-11 class, D13).
  `SIZE < 2^ABITS` maps (the RAM is a superset); `ABITS 0` maps with all 15 address pins on the pad.
- (f) **A port clocked on the falling edge** (`*_CLK_POLARITY` 0) leaves the `$mem` soft (R-11 class, D14):
  the arch's RAMs have no clock inversion and `lower`'s inverter rule is for FFs.
- (g) **Hard cell parameters** are node parameters created with the cell (`WIDTH`, `ADDR_WIDTH`,
  `DATA_WIDTH`); the glue kit is `hm_and/hm_not/hm_mux/hm_dff(hc, ...)` returning a fresh net; they are
  the only allocation points of a builder, so the OOM sweeps hook them.
- (h) **Divergence rows for 3H:** Task 7 adds one `divergences.tsv` class, `dual-write-race`, for
  `LU8PEEng` and `LU32PEEng` (R-6). D1 and D4 need no row (Odin III passes the functional check where
  Parmys fails); 3H's research lists Parmys's `baseline-fails`.
- (i) **Role of the probes:** the two Parmys defects of spec questions 5 and 6 were confirmed by Peter's
  probe (`odin3-2D1/.superpowers/sdd/3C-probes/README.md`: live-address mux 468 mismatches in 2000 cycles
  with `addr[0]` held constant giving 0; subtraction top stage loses the borrow bit). D2b and D4 are
  therefore recorded; Task 1 makes the evidence reproducible in the repo.

## Needs Peter

1. **Conditional, probably none.** No IR or invariant change is planned (attributes, DERIVED records,
   `odin3_net_merge` and node parameters all exist). The one place that could need a ruling: Task 3's
   test `a7_y_alias_ports` merges two Y bits onto one net, which may be two module output ports. If
   `odin3_check_design` rejects that, stop and ask (CLAUDE.md rule 10) whether `check` may allow a net
   driving two output ports (3A's `flat_feedthrough` relies on the same property). Expected outcome: it is
   already allowed, and nothing is asked.

Everything else follows PHASE3 #32; the agent defaults above are what Task 7 logs.

---

## Task 1: Parmys probes and `parmys-probe` (spec §9.2; PR 3C-1)

**Files:**
- Create `tools/hardmap-check/parmys-probe` (shell wrapper) and `tools/hardmap-check/parmys_probe.py` (typed, stdlib; the BLIF-to-Verilog harness of the 2D1 `probe.py`: hard blocks become `vtr_flow/primitives.v` instances, `unconn` is the BLIF's own constant-0 `.names`, an iverilog testbench against the source, inputs changing after each clock edge, outputs sampled before the next, RAM warmed by 16 writes, `prlimit` 3 GiB).
- Create `tests/hardmap/probes/{ramsplit,subeq,subwide,subprobe,addrmerge}.v`, `tests/hardmap/probes/README.md` (copied from `odin3-2D1/.superpowers/sdd/3C-probes/`, plus `addrmerge.v` and the recorded results).
- Create `tests/tools/test_parmys_probe.py`; modify `CMakeLists.txt` (the Python test; the probe legs as `YOSYS055`+`iverilog` gated CTests `parmys_probe_*`, label `sweep`, via `odin3_add_sweep_test`).

**Interfaces:**
- Produces: `parmys-probe [--cycles N] [--warm N] [--mask port=0xH] [--corner] SRC.v TOP PARMYS.blif OUTDIR` with exit 0 (no mismatch), 1 (mismatch, first one printed as `cycle N: <port> ref <v> blif <v>`), 2 (usage/tool error), 77 (no iverilog). `blif_to_verilog(text: str) -> str` and `run_probe(args) -> int` are importable for the unit test.
- Consumes: goldens made by `tools/run-oracle.sh --tool parmys --golden <scratch> --name X X.v k6_frac_N10_frac_chain_mem32K_40nm.xml` (scratch root, never `~/odin3-ws/golden`).

- [ ] **Step 1: Write the failing unit tests** (`test_parmys_probe.py`): `test_blif_to_verilog_names` (an `.names` constant 0 becomes `assign n = 1'b0;`; `x~3` bit names group into a bus), `test_blif_to_verilog_subckt` (a `.subckt adder a=.. b=.. cin=.. cout=.. sumout=..` becomes a primitive instance), `test_probe_exit_codes` (a planted matching BLIF exits 0, a BLIF with one flipped LUT row exits 1 and prints the first cycle, a missing iverilog exits 77 via a fake PATH).
- [ ] **Step 2: Run them; they fail** (`.venv/bin/python -m unittest tests/tools/test_parmys_probe.py`).
- [ ] **Step 3: Implement** the tool by porting `probe.py` with type annotations (`ruff` and `mypy --strict` clean), exit-code contract above.
- [ ] **Step 4: Reproduce the confirmed defects.** Run Parmys on `ramsplit.v`, `subeq.v`, `subwide.v`, `subprobe.v`, then `parmys-probe`. Expected (Peter's run): `ramsplit` 2000 cycles with 468 mismatches, first at cycle 23, and 0 with `addr[0]` held constant (`--mask addr=0x000e`, as in the README); `subeq` 0 of 3000; `subwide` mismatches with `uy9` ref `18b`, blif `08b` (borrow lost). Quote the output in the README (D4 and D2b are now recorded).
- [ ] **Step 5: New probe `addrmerge.v`** (agent default d): `always @(posedge clk) begin if (we) m[wa] <= d; q <= m[ra]; end` with the read address registered first (`ra_q <= ra; out = m[ra_q]`), same-address writes in the stimulus. Run it through Parmys and `parmys-probe`; record in the README whether the Parmys netlist matches the RTL. Mismatch -> D12 stands as written in agent default (d); match -> delete D12 and keep spec R-8, in the same README and in Task 6's tests.
- [ ] **Step 6: Gate and commit.** `tools/lint.sh`; `feat(hardmap-check): parmys-probe and the 3C probe micros (3C §9.2)`; push.

## Task 2: Scaffold, glue kit and `$mul` (H-1..H-10, M-1..M-7; PR 3C-1)

**Files:**
- Create `src/passes/hardmap/{hardmap.h,register.c,hardmap.c,hm_kit.h,hm_kit.c,mul.c}`, `tests/unit/hardmap_fixture.[ch]` (builders for flat IR with the vtr techlib read; a `hm_sim_equal(build_fn, vectors, seed)` helper that builds twice, runs `hardmap` on one and compares the 1E simulator traces), `tests/unit/test_hardmap_mul.c`, `tests/hardmap/mul_8x7.v`, `tests/hardmap/expected/mul_8x7.blif`.
- Modify `src/passes/builtin.c`, `src/passes/builtin.h` (one table entry `&odin3_pass_hardmap`, append-only beside `flatten`/`lower`), `CMakeLists.txt` (unit test, `cli_hardmap_mul`), `docs/PASSES.md` (one row; golden-test column completed in Task 7).

**Interfaces:**
- Consumes: `odin3_celltype_find`, `odin3_node_create_connected`, `odin3_node_delete`, `odin3_net_create`, `odin3_net_merge`, `odin3_attr_set`, `odin3_prov_begin_op`, `odin3_prov_derive`, `odin3_opt_both_signed`, `odin3_budget_check`, `odin3_pass_register_def` fields (`processes = ODIN3_PASS_PROCESSES_REFUSE`).
- Produces (`hm_kit.h`): `typedef struct hm_ctx hm_ctx;` (design, module, pass ctx, op, stats, type cache, created-pin counter); `typedef enum { HM_ADDER, HM_MULTIPLY, HM_SP_RAM, HM_DP_RAM } hm_hard;` `odin3_status hm_need(hm_ctx *hc, hm_hard which, odin3_celltype_id *out)` (H-3 error, once per type); `odin3_status hm_begin_cell(hm_ctx *hc, odin3_node_id src)` (one operation, one DERIVED record); `odin3_status hm_new_node(hm_ctx *hc, odin3_celltype_id type, const odin3_value *params, uint32_t n_params, const odin3_netvec *ports, const char *role, odin3_node_id *out)` (name and `hardmap` attribute per agent default b; budget check every 2^16 pins); glue `odin3_status hm_and/hm_not/hm_mux/hm_dff(...)` returning a fresh `odin3_net_id`; `odin3_net_id hm_const(hm_ctx *hc, hm_bit v)` with `HM_GND`, `HM_VCC`, `HM_PAD` (the `$_CONSTX_` net); `odin3_status hm_commit(hm_ctx *hc, odin3_node_id src, const hm_tie *ties, size_t n)` (reserve, delete the source, merge `keep` = the source's output net, `drop` = the fresh net); `odin3_status hm_map_mul(hm_ctx *hc, odin3_node_id src)` in `mul.c`.

- [ ] **Step 1: Write the failing tests** (`test_hardmap_mul.c`; each builds flat IR, runs `odin3_hardmap_run`, asserts pins and `check` FULL):
  - `h3_missing_type` (no techlib: the error line verbatim, IR unchanged), `h3_not_hard` (a user cell named `multiply` that is not `hard`).
  - `m2_threshold` table (A,B,signed,Y -> result): (2,2) soft + WARN; (1,1), (1,5), (2,32), (32,2), (3,3), (36,36) hard; (37,4), (4,37) soft + WARN; signed (18,18,Y 36) hard; signed Y 37 soft + WARN; mixed-sign cell takes the unsigned path (PHASE2 #25); `A_WIDTH 0` untouched, no log.
  - `m3_shape_8x7`: one `multiply` `WIDTH=36`; `a[0..7]` = A, `a[8..35]` = `HM_PAD`; `b[0..6]` = B, `b[7..35]` = `HM_PAD`; `out[0..14]` drive Y; `out[15..71]` are 57 fresh nets with no reader; no `$mul` left.
  - `m3_y_wider`: A 4, B 4, Y 12: Y[8..11] on gnd.
  - `m4_pad_is_x`: pad nets are the `$_CONSTX_` net; `cli_hardmap_mul` (`odin3 -p "read_techlib lib/vtr.o3lib; read_verilog --vtr-compat tests/hardmap/mul_8x7.v; proc; opt; hardmap; write_blif --vtr out.blif"`) equals the committed BLIF, which writes the 28+29 pad pins as `unconn`.
  - `m5_signed_sim`: signed 3x3 -> Y 6 and 4x4 -> Y 8, exhaustive over all input pairs, `hm_sim_equal`; operand bits above `a` equal `A[a-1]` up to `Y_WIDTH`, pad above.
  - `m1_const_operand` (a constant operand still goes hard), `m6_explicit_multiply` (WIDTH 36 and 8 untouched, node ID unchanged; WIDTH 40: `hardmap: 'multiply' instance 'u' has WIDTH 40; the arch's multiplier is 36 wide`, a non-OK status, IR unchanged), `m7_order_naming` (three `$mul`s mapped in node-ID order, the node takes the cell's name, `hardmap` attribute `multiply:mul`), `h7_prov` (one operation, DERIVED parent = the source's record), `h9_oom` (allocation sweep over one mapping: `NO_MEMORY`, IR valid, `$mul` intact), `h8_determinism` (two runs, byte-identical `write_rtlil`), `h10_info` (`hardmap: 3 multiply, 0 adder in 0 chains, 0 single_port_ram + 0 dual_port_ram slices; 2 cells left soft`), `hardmap_twice` (second run maps nothing), `refuse_processes` (a module with `$proc` -> manager refusal).
- [ ] **Step 2: Run them; they fail** (`ctest --preset debug -R "hardmap"`).
- [ ] **Step 3: Implement** `register.c` (pass `hardmap`, `processes = REFUSE`, no arguments: `hardmap: takes no arguments`), `hardmap.c` (module loop, kind order, stats, INFO/WARN lines), `hm_kit.c`, `mul.c` (M-1..M-7). Each function inside the lizard gate.
- [ ] **Step 4: Gate.** `tools/build-locked --preset debug -j6 && ctest --preset debug -j4`; `check` FULL before/after on each test is automatic in Debug.
- [ ] **Step 5: `/run-micro`** on `common/mult.v` and `micro/multiply_hard_block.v` through the script up to `hardmap` (no regression; hard `multiply` untouched).
- [ ] **Step 6: Lint, commit, push, open PR 3C-1.** `feat(hardmap): scaffold, glue kit and $mul mapping (3C M-1..M-7)`; whole-branch critique first (CLAUDE.md rule 12); self-merge on green `ci`.

## Task 3: `$add`/`$sub` -> `adder` chains (A-1..A-9; PR 3C-2)

**Files:**
- Create `src/passes/hardmap/adder.c`, `tests/unit/test_hardmap_adder.c`, `tests/hardmap/sx_add.v` (copy of `odin3-2D1/.superpowers/sdd/2026-10-11-2D-elaboration-plan/exit-repros/sx_add/sx_add.v`), `tests/hardmap/sub_pad_{u,s}.v`.
- Modify `src/passes/hardmap/hardmap.c` (dispatch), `CMakeLists.txt`, `docs/PASSES.md`.

**Interfaces:**
- Consumes: Task 2's kit. Produces: `odin3_status hm_map_adder(hm_ctx *hc, odin3_node_id src)`; static helpers `ext_bit(...)` (A-2's `ext(X)[i]`), `inv_bit(...)` (constant fold, else one shared `$not` per distinct B net), `build_chain(...)`.

- [ ] **Step 1: Write the failing tests** (`test_hardmap_adder.c`):
  - `a3_chain_len`: (A,B) = (1,1) -> 3 adders, (8,8) -> 10, (3,6) -> 8; `Y_WIDTH` 1 leaves the count unchanged and connects only `Y[0]`.
  - `a4_add_pins`: unsigned 4+4 into Y 5: head `a=vcc b=gnd cin=gnd`, `sumout` unread; stage i `a=A[i] b=B[i]`, `cin` = previous `cout`; top stage `a=b=gnd`; `Y[i]` = `sumout_i` for i <= 4; top `cout` unread.
  - `a2_ext`: unsigned A 3 / B 6: A bits 3..5 on gnd; signed: on `A[2]`.
  - `a5_sub_pins`: head `a=b=vcc`, `cin` = pad; stage `b` = `~ext(B)`; constant B bits fold to vcc/gnd with no `$not`; a repeated sign bit gets exactly one `$not`; unsigned `A 8 - B 5 -> Y 9`: stages 5..8 have `b = vcc`.
  - `a8_sub_top_stage`: unsigned 8-bit `ua - ub` into 9 bits, exhaustive simulation against the RTL semantics, `Y[8]` is the borrow (1 when `ua < ub`). This is the case Parmys gets wrong (Task 1 probe `subwide`).
  - `a6_sx_add`: the `sx_add` cell after 3G (signed, A 9, B 9, Y 11, built by hand), 4096 seeded vectors plus corners, `hm_sim_equal`, and a second simulation of `tests/hardmap/sx_add.v` through `read_verilog; proc; opt; wreduce; hardmap` against iverilog's trace for `Y[9]`, `Y[10]`.
  - `a7_y_wider`: signed or `$sub` -> `Y[n+1..]` alias `Y[n]`; unsigned `$add` -> gnd; `a7_y_alias_ports` (two Y bits are module outputs: `check` FULL passes; Needs Peter item 1 otherwise).
  - `adder_sim_matrix`: widths 1..4 x {signed, unsigned} x {`$add`, `$sub`} x `Y_WIDTH` in {n, n+1, n+2}, exhaustive, `hm_sim_equal`.
  - `a1_candidates`: constant operands go hard; `$neg`, `$lt` and `$add` with both widths 0 are untouched; `a9_explicit`: a 1-bit `adder` instance untouched (node ID unchanged), `adder #(4)` from `vtr_primitives.v` after `flatten` becomes `$add` and maps.
  - `a_alias_operands`: `a + a`, `a - a` and `x + x` with a shared sign net simulate correctly.
  - `h7_prov_chain` (all n + 2 adders and the `$not`s share one DERIVED record, parent = the source), `h9_oom` over one chain, `m_adder_names` (`<cell>`, `<cell>.head`, ... as agent default b), `hardmap_twice`.
- [ ] **Step 2: Run them; they fail.**
- [ ] **Step 3: Implement** `hm_map_adder` per A-1..A-9 in node-ID order after `$mul`. A-8: follow A-5 (D2b is confirmed by Task 1).
- [ ] **Step 4: Gate.** Build and `ctest --preset debug -j4`.
- [ ] **Step 5: Lint and commit** `feat(hardmap): adder and subtractor chains (3C A-1..A-9)`; push.

## Task 4: `hardmap-diff` and the micros (spec §9.3; PR 3C-2)

**Files:**
- Create `tools/hardmap-check/hardmap-diff`, `tools/hardmap-check/hardmap_diff.py`, `tools/hardmap-check/phase3_pre_abc.script`, `tests/tools/test_hardmap_diff.py`, `tests/hardmap/diff_expect.txt`, `tests/hardmap/micros.txt` (the arithmetic and memory micros: `common/{adder,sub,mult,mult_const,mem,rom,spram,dpram,bram,1r,2r,1r2w,2r1w,2r2w,nr,nrnw}.v`, `micro/{adder_hard_block,multiply_hard_block,hard_adder_cin_propagation}.v`, `operators/*arith*`, `full/{ch_intrinsics,mkPktMerge,stereovision0,stereovision1}.v`).
- Modify `CMakeLists.txt` (`hardmap_diff_micro`, label unit-or-skip; `hardmap_diff_unit`).

**Interfaces:**
- Produces: `hardmap-diff [--odin3 P] [--yosys P] [--golden-root D] [--arch A] [--expect FILE] [--keep DIR] DESIGN.v...` per design: run the pre-ABC script, parse hard-block `.subckt` counts by model from both BLIFs (`adder`, `multiply`, `single_port_ram`, `dual_port_ram`, plus `.latch` count), then `netlist-compare` and, if different, `equiv-check`; classes `identical`, `equivalent`, `listed:<Dn>`, `counts-differ`, `differs`, `unproven`, `error`; exit 0 only when every design is identical, equivalent or listed. `tests/hardmap/diff_expect.txt` lines: `<path> <D-id> <reason>`; a listed design that now matches is stale and fails (the list shrinks, as `tests/exit/*`). Count exceptions use `counts:+6 latch` form (`mcml`).
- Consumes: Task 2-3 pass; the script; `tools/netlist-compare`, `tools/equiv-check` (read only); goldens `golden/k6_frac_N10_frac_chain_mem32K_40nm/<group>/<d>/<d>.parmys.blif`.

- [ ] **Step 1: Write the failing tests** (`test_hardmap_diff.py`, fake odin3 and fake compare tools): `identical`, `equivalent`, `counts-differ` (one adder missing), `listed` with a reason, a stale entry fails, an unlisted difference fails, exit codes 0/1/2, `--keep`, the `counts:+6 latch` form, `unproven` for `equiv-check` exit 4.
- [ ] **Step 2: Run them; they fail.**
- [ ] **Step 3: Implement** the tool (stdlib, `ruff`, `mypy --strict`); the script file is the single source of the Phase 3 string.
- [ ] **Step 4: Run the real legs on the micros** (`hardmap_diff_micro`; skipped with exit 77 until `lower` is on `main`). Expected: unsigned arithmetic micros `identical` or `equivalent`; signed micros differ only where `diff_expect.txt` lists D1/D3 with a reason; memory micros of Tasks 5-6 are listed as they land. Each listed difference is checked by hand against the RTL with `parmys-probe` (the tool is the check, not the expect file).
- [ ] **Step 5: Gate, lint, commit** `feat(hardmap-check): hardmap-diff and the arithmetic micros (3C §9.3)`; push; whole-branch critique; open PR 3C-2; self-merge on green `ci`.

## Task 5: RAM instances (R-1..R-7, H-4, H-7; PR 3C-3)

**Files:**
- Create `src/passes/hardmap/ram.c` (`hm_map_ram`: depth split, width split, address padding), `src/passes/hardmap/ram_split.c` (the level loop and select/mux glue), `tests/unit/test_hardmap_ram.c`.
- Modify `hardmap.c` (dispatch: RAM instances after `$add`/`$sub`), `CMakeLists.txt`, `docs/PASSES.md`.

**Interfaces:**
- Consumes: Task 2 kit, `hm_and/hm_not/hm_mux/hm_dff`. Produces: `odin3_status hm_map_ram(hm_ctx *hc, odin3_node_id src)` for a `single_port_ram` or `dual_port_ram` node whose `ADDR_WIDTH > 15` or `DATA_WIDTH > 1`; static `split_depth(...)` (explicit level loop, lowest address bit first), `split_width(...)`, `pad_addr(...)`. A node already at (15, 1) is untouched.

- [ ] **Step 1: Write the failing tests** (`test_hardmap_ram.c`):
  - `r3_width_split`: SP `ADDR 15 DATA 8` -> 8 `single_port_ram` nodes `ADDR_WIDTH=15 DATA_WIDTH=1`, slice j gets `data[j]`/`out[j]`, shared `addr`, `we`, `clk`; DP the same with `data1/data2/out1/out2`.
  - `r4_addr_pad`: `ADDR 10`: pins 10..14 on `HM_PAD`; `ADDR 1`: 14 pad pins; the writer prints `unconn`.
  - `r2_depth_16`: SP `ADDR 16 DATA 1` -> 2 RAMs on `addr[15:1]`; `we_H = we & ~addr[0]`, `we_S = we & addr[0]` (`$and`/`$not` of width 1); `out = $mux(sel, out_H, out_S)` with `sel` = a 1-bit `$dff(clk, addr[0])` (R-7); `r2_depth_17`: `DATA 2` -> 8 RAMs, 2 select FFs (levels 0 and 1, shared by the data bits), 6 `$mux`.
  - `r7_dp_depth`: DP `ADDR 16`: each port splits on its own address bit, 2 select FFs (one per read port), both halves carry both ports.
  - `r7_select_registered_sim`: SP `ADDR 16` and DP `ADDR 16`, 2000 seeded cycles with `addr[0]` changing every cycle, `hm_sim_equal` (this fails with a live-address select, which is how D4 is pinned); `r7_select_constant` (`addr[0]` held constant: also equal).
  - `r5_const_we_kept`: `we` on gnd stays wired to gnd, the RAM stays.
  - `r6_two_live_writes`: DP with live `we1` and `we2` -> only `dual_port_ram` slices, no `$and`/`$mux`/`$dff` added (cell histogram), both write ports on every slice.
  - `r1_order`: `ADDR 16 DATA 4` gives depth-first then width: 8 RAMs with `ADDR_WIDTH=15`, `DATA_WIDTH=1`, one select FF for the single read port (assert counts: 8 RAMs, 1 `$dff`, 4 `$mux`).
  - `h7_prov_ram` (every slice and glue node has the instance as DERIVED parent), `r_names` (`<inst>.b5.h` style per agent default b), `h9_oom`, `hardmap_twice`, `r_big` (`ADDR 16 DATA 36`, as `mcml`: 72 RAMs, 1 FF per port).
- [ ] **Step 2: Run them; they fail.**
- [ ] **Step 3: Implement** R-1..R-7 (a loop over levels, no recursion; the select FF per (level, read port)).
- [ ] **Step 4: Gate, lint, commit** `feat(hardmap): explicit RAM instances, depth and width split (3C R-1..R-7)`; push.

## Task 6: Inferred `$mem` (R-8..R-13; PR 3C-3)

**Files:**
- Create `src/passes/hardmap/mem_plan.c` (classify ports, choose SP/DP/replicas/soft, R-10), `src/passes/hardmap/mem_merge.c` (R-8 FF merge), `src/passes/hardmap/mem.c` (`hm_map_mem`), `tests/unit/test_hardmap_mem.c`.
- Modify `hardmap.c`, `CMakeLists.txt`, `tests/hardmap/diff_expect.txt`.

**Interfaces:**
- Consumes: Tasks 2 and 5 (`hm_map_ram` builds the slices from a synthesized RAM description `hm_ram_desc { hm_hard kind; uint32_t abits, width; odin3_netvec ports... }`). Produces: `odin3_status hm_map_mem(hm_ctx *hc, odin3_node_id src)`; static `plan_ports(...)` returning `hm_plan { SOFT, SP, DP, SP_REPLICAS, DP_REPLICAS }`, `merge_read_ports(...)`.

- [ ] **Step 1: Write the failing tests** (`test_hardmap_mem.c`, `$mem` built as 2D EL-39 emits it: asynchronous reads, raw addresses, `RD_EN` 1):
  - R-8: `r8_data_merge` (each `RD_DATA` bit feeds one `$dff` on one clock: FFs removed, port synchronous, `RD_TRANSPARENT 0`, then mapped); `r8_data_merge_dffe` (enable becomes `RD_EN`; non-constant -> the `$mem` stays soft with the R-10 warning and its **original FFs**, agent default d); `r8_addr_merge_no_write` (address FFs bypassed, read-only memory); `r8_addr_merge_with_write` (stays asynchronous: R-9 warning, or merged per the Task 1 probe result); `r8_mixed_clocks_no_merge`.
  - R-9: `r9_async_stays_soft` (warning verbatim `hardmap: memory '<m>' has an asynchronous read port; left to lower as flip-flops`, `$mem` node unchanged, no RAM created); `r9_multi_clock`; `r9_negedge_soft` (agent default f).
  - R-10 pattern tests, one per row: `p_1_0` (SP, `we` gnd, `data` pad), `p_2_0` (DP, `we1 = we2 = gnd`), `p_3_0` (3 SP replicas, one read port each), `p_1_1_same_addr` (SP), `p_1_1_diff_addr` (DP: write port 1, read port 2), `p_1_2`, `p_2_1` (the read with the identical address goes with the write on port 1; with neither identical: two DP replicas), `p_2_2` (each port a write plus the read of identical address; otherwise soft + R-11 warning), `p_3_1` (replicas), `p_3_2` and `p_1_3` (soft + R-11 warning), `p_rd_en_nonconst` (soft with the warning). Each asserts the created cell histogram and the pins.
  - `r11_warning_text`, `r12_init_dropped` (`hardmap: memory '<m>': initial contents dropped (the arch's RAMs have no init)`), `e_offset_nonzero_soft` (agent default e), `e_size_below_pow2_maps`, `e_abits0_maps` (15 address pins on the pad).
  - `mem_sim`: 2000 seeded cycles, `hm_sim_equal` for `common/mem.v`'s shape (data merge, read-first), a ROM, a 1r1w and a 2r1w, `ABITS` 4 and 16 (the latter exercises the select FF), same-address read/write collisions included for non-transparent ports.
  - `h7_prov_mem`, `h9_oom`, `hardmap_twice`, `h8_determinism`.
- [ ] **Step 2: Run them; they fail.**
- [ ] **Step 3: Implement** R-8 (merge into a transaction that is dropped when the `$mem` ends soft), R-9..R-13 and the pattern table; the physical mapping goes through `hm_map_ram`.
- [ ] **Step 4: `/run-micro`** on the memory micros through `hardmap-diff` (Task 4); list the expected D5/D8/D9/D12-D14 differences in `diff_expect.txt` with reasons, each checked with `parmys-probe` where the RTL decides.
- [ ] **Step 5: Gate, lint, commit** `feat(hardmap): inferred memories, FF merge, port patterns (3C R-8..R-13)`; push.

## Task 7: The 19, divergences, docs (spec §9.4-§9.6, §10, §11; PR 3C-3)

**Files:**
- Create `tests/hardmap/vtr_expect.txt` (`<design> <D-id|counts:...> <reason>`), `tests/hardmap/README.md`.
- Modify `CMakeLists.txt` (`hardmap_vtr_sweep` through `odin3_add_sweep_test`, label `sweep`), `tests/vtr/qor/divergences.tsv` (rows `LU8PEEng` and `LU32PEEng`, class `dual-write-race`, text `3C R-6: two live write ports to one address are an IEEE race`; the file and `test_divergences_rows`' class list belong to 3H Task 11, so append-only, and if 3H-3 is not yet merged the file is created here and 3H keeps both), `docs/PASSES.md` (final row: rules, files, tests), `docs/PHASE3.md` (checklist 3C, decisions entry with the agent defaults a-i, the sweep table verbatim, exit item 3's script string), `docs/specs/2026-10-14-3C-hardmap-design.md` (the §11 amendments), `docs/DESIGN.md` only where §7 names the fixed subset.

- [ ] **Step 1: Write the failing tests.** `hardmap_vtr_sweep` lists the 19 as missing expectations; `test_divergences_rows` accepts the new class.
- [ ] **Step 2: The 19 through `hardmap-diff` (controller, heavy, smallest first, idle machine).** `ODIN3_HEAVY_OK=1 tools/heavy-run tools/hardmap-check/hardmap-diff --odin3 build/release/odin3 --yosys $VTR_ROOT/build/bin/yosys --arch $VTR_ROOT/vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml --expect tests/hardmap/diff_expect.txt --expect tests/hardmap/vtr_expect.txt <d>.v` for, in this order: `ch_intrinsics stereovision3 diffeq2 diffeq1 mkPktMerge raygentop sha boundtop mkSMAdapter4B or1200 stereovision0 stereovision1 mkDelayWorker32B stereovision2 blob_merge LU8PEEng bgm mcml LU32PEEng` (`ctest --preset sweep -R hardmap_vtr_sweep` runs the same list). Gate: every design `identical`, `equivalent` or `listed`; designs in `tests/exit/vtr_unproven.txt` (`stereovision3`, `LU32PEEng`, `mcml`) fall back to the count check.
- [ ] **Step 3: Hard-block counts equal the golden's** (spec §2 table): `multiply`, `adder`, SP and DP slices on all 19. Exceptions are only `mcml` (+6 FFs, D4) and the D1 designs' pin differences with equal counts. Anything else is a finding to fix with a unit test first, not a list entry.
- [ ] **Step 4: The functional-check inputs.** Verify the declared-divergence list per design (spec §9.6): D1 on `stereovision0/1/2`, `LU8PEEng`, `LU32PEEng`, `mcml` (Odin III must be RTL-correct where Parmys is not: run `parmys-probe`-style iverilog on `tests/hardmap/sx_add.v` and the `stereovision0` eq-x cone), D4 on `mcml`, R-6 on the two `LU` designs (the divergences row).
- [ ] **Step 5: The O' leg (3H).** If 3D-5 (`abc`) and 3H-3/4 are merged: `ODIN3_HEAVY_OK=1 tools/heavy-run tools/vtr-qor run --leg O\' --designs <the same order>`; every design must reach `vpr_status=success`, `num_mult` and `num_memories` equal P's. Otherwise record "M2 waits for 3D-5/3H" in the PR; 3H Task 15 runs M2.
- [ ] **Step 6: Docs and critique.** The amendments of spec §11, the PHASE3 entry (agent defaults a-i, the Task 7 table), `tools/lint.sh` (it checks the docs rules), whole-branch critique, triage, list rejected findings.
- [ ] **Step 7: Commit, push, open PR 3C-3** "3C: hardmap RAM, inferred memory and the 19" (body: spec and plan links, the sweep table, the divergence table, rejected findings); self-merge on green `ci`.

## Self-review

- **Spec coverage:** H-1/H-2 (script position) -> Global Constraints and Task 4's script file; H-3 -> Task 2; H-4 -> Tasks 2, 5 (parameters, the `unconn` CLI test); H-5 -> Task 2 (`HM_*` consts); H-6 -> Task 2 kit and Task 5 glue; H-7 -> `h7_prov*` tests in Tasks 2, 3, 5, 6; H-8/H-9/H-10 -> Tasks 2-6 (`determinism`, `oom`, `info`); M-1..M-7 -> Task 2; A-1..A-9 -> Task 3; R-1..R-7 -> Task 5; R-8..R-12 -> Task 6 (R-13 and D12-D14 are the plan's additions); S-1..S-4 -> the soft assertions of Tasks 2, 3, 6; §9.1 rule tests -> Tasks 2, 3, 5, 6; §9.2 -> Task 1; §9.3 -> Task 4; §9.4/9.5/9.6 -> Task 7; §10 D1-D11 -> asserted per task, D12-D14 new; §11 amendments -> Task 7.
- **Open spec points closed above:** `OFFSET`, falling-edge ports, merge scope, naming. None changes a rule Peter approved; (d) rests on Task 1's probe.
- **Type consistency:** `hm_ctx`, `hm_need`, `hm_begin_cell`, `hm_new_node`, `hm_commit`, `hm_const` and the glue names are defined in Task 2 and used unchanged in Tasks 3, 5, 6; `hm_map_ram` (Task 5) is the only constructor of RAM slices and is called by Task 6.
- **Proportion:** the plan lists tests and signatures, not bodies. Sweeps the controller runs: Task 7 Steps 2 and 5, under the heavy lock.
