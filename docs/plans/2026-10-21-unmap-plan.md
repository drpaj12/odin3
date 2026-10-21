# `unmap` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new pass `unmap` that turns the VTR hard cells (`adder`, `multiply`, `single_port_ram`, `dual_port_ram`) back into `$add`/`$sub`, `$mul` and `$mem` cells with provenance, regrouping carry chains, split multipliers and sliced RAMs, exactly or not at all.

**Architecture:** One pass, `src/passes/unmap/`, per module and in node-ID order, phases A (adder chains), B (`multiply` to `$mul`), C (multiplier regroup by algebraic pattern), D (RAM cells grouped by shared provenance, then converted), F (depth glue recovery). Every group is replaced by one `um_replace` transaction (one DERIVED provenance record over the old nodes' records, like `raise`). Net-only rules, so a VTR BLIF with no provenance works too.

**Tech Stack:** C17 (CLAUDE.md limits), Unity unit test, `tests/cli/test_cli.sh` golden cases, `tools/equiv-check`, Python stdlib for the sweep tool.

**Spec:** `docs/specs/2026-10-21-unmap-design.md` (rule IDs C-n, M-n, R-n, P-n, E-n, S-n, N-n, T-n below refer to it). Read it and `docs/specs/2026-10-20-4.2-partial-map-design.md` sections 3 to 5 first. The code to invert is `src/passes/partial_map/{adder,mul,mul_split,ram,ram_split,mem,mem_plan,hm_kit}.c`; the pattern for a clump replacement is `src/passes/raise/raise.c` (`make_prov`, `create_cell`, `replace_group`).

## Global Constraints

- Branch `feat/unmap` (a new branch from `origin/main`; this spec branch is `feat/unmap-spec`). One pass per PR: unit test `tests/unit/test_unmap.c`, golden cases, `docs/PASSES.md` row.
- C17, flags `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes -Wvla -Werror`; functions at most 60 lines, cyclomatic complexity at most 15, at most 5 parameters; no VLAs, no `goto` except one `cleanup:`; no recursion in IR traversals (explicit worklists); no `NOLINT`; named constants (no magic numbers outside `tests/`).
- Errors return `odin3_status`; no `exit()`; no `malloc` of IR objects; containers only from `src/util/` (`odin3_vec`, `odin3_u64map`, `odin3_strbuf`).
- `check` runs before and after the pass (the manager does it); never disable it.
- Run `tools/lint.sh` before every commit; commit and push after every task (`git push -u origin feat/unmap` the first time).
- Never read the `hardmap` attribute or node names in a rule (S-2); only for ordering ties in R-6 and in tests.
- Output is deterministic: node-ID order only, no hash order.
- Build and test: `cmake --preset debug && cmake --build --preset debug`, `ctest --preset debug -R <name> --output-on-failure`. Equivalence tools: `tools/equiv-check/equiv-check a.blif b.blif` (0 equivalent).
- No em-dashes or emojis in code, docs or messages.

## Review Focus

Failure modes the spec implies that no single rule test covers; each has its test in the task named.

1. A carry chain whose last `cout` also feeds logic, and one whose middle `cout` feeds logic and the next chain: the first part is recovered with the extra `Y` bit, the rest declined with `cin_carry` and no net lost (Task 5).
2. A `multiply` with every operand pin `unconn` or open, and one whose outputs nobody reads: converted with `Y_WIDTH = ua + ub`, no crash, no `Y_WIDTH` of 0 (Task 6).
3. A split multiplier where one intermediate product net is also read elsewhere: no merge, note `mul:shared`, both readers still driven (Task 8).
4. Two RAM cells with equal control nets and different origin (BLIF) are not merged and carry `unmap_cand`; the same pair from one `hardmap` run are merged (Task 10).
5. Running `unmap` twice, on a multi-module design, and on a design with no hard cell or no `lib/vtr.o3lib` loaded: second run changes nothing, modules are independent, no error (Task 11).

---

### Task 1: Scaffold, registration, options

**Files:**
- Create: `src/passes/unmap/unmap.h`, `src/passes/unmap/register.c`, `src/passes/unmap/unmap.c`
- Create: `tests/unit/test_unmap.c`
- Modify: `CMakeLists.txt` (add the three sources after `src/passes/raise/register.c` at line 120; add `odin3_add_unit_test(test_unmap)` after the `test_raise` block near line 1755, with `target_compile_definitions(test_unmap PRIVATE ODIN3_UM_ROOT="${PROJECT_BINARY_DIR}" ODIN3_UM_LIB="${PROJECT_SOURCE_DIR}/lib/vtr.o3lib")`)
- Modify: `src/passes/builtin.h` (extern after `odin3_pass_raise`, line 42), `src/passes/builtin.c` (append `&odin3_pass_unmap` to `odin3_builtin_passes`, line 855)
- Modify: `docs/PASSES.md` (a row after `raise`)

**Interfaces:**
- Produces (`src/passes/unmap/unmap.h`):
  ```c
  typedef struct um_opts { bool regroup; } um_opts;            /* default true; --no-regroup clears */
  typedef struct um_stats {
      uint64_t adder_cells, add, sub, multiply, mul, regrouped_mul, ram_cells, mem, declined, cands;
  } um_stats;
  odin3_status um_run(odin3_pass_ctx *ctx, odin3_design *design, const um_opts *opts,
                      um_stats *stats);                        /* stats may be NULL */
  extern const odin3_pass_def odin3_pass_unmap;                /* register.c */
  ```
  `um_run` loops `mid = 1 .. odin3_design_module_end(design)` and calls `um_module(um_ctx *uc)` (declared in `unmap.h`, defined in `unmap.c`; Task 1 body: nothing yet).

- [ ] **Step 1: Write the failing test** in `tests/unit/test_unmap.c` (copy the `setUp`/`tearDown`/`sink`/`read_blif`/`check_clean` scaffolding of `tests/unit/test_raise.c` lines 47 to 100 and 168 to 172; `ODIN3_UM_ROOT` replaces `ODIN3_RS_ROOT`). Add a helper `static void load_lib(void)` running `odin3_pass_run(design, "read_techlib", odin3_bytes_cstr(ODIN3_UM_LIB))` and a helper `static odin3_status unmap(const char *args)` = `odin3_pass_run(design, "unmap", odin3_bytes_cstr(args))`. Test `test_unmap_options`:
  ```c
  TEST_ASSERT_NOT_NULL(odin3_pass_find(odin3_bytes_cstr("unmap")));
  TEST_ASSERT_EQUAL_INT(ODIN3_OK, unmap(""));
  TEST_ASSERT_EQUAL_INT(ODIN3_OK, unmap("--no-regroup"));
  TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, unmap("--bogus"));
  TEST_ASSERT_NOT_NULL(strstr(log_text, "unmap: unknown argument '--bogus' (known: --no-regroup)"));
  ```
  and `main` with `RUN_TEST(test_unmap_options)`.
- [ ] **Step 2: Run it to see it fail:** `cmake --preset debug && cmake --build --preset debug --target test_unmap` fails (no sources); expected.
- [ ] **Step 3: Implement.** `register.c`: `static odin3_status unmap_pass_run(odin3_pass_ctx*, odin3_design*, odin3_bytes args, void *user)` parsing with `odin3_pass_arg_next` and `odin3_pass_arg_is` exactly as `src/passes/partial_map/register.c` lines 66 to 91, the error text of S-4, then `um_run`; `const odin3_pass_def odin3_pass_unmap = {"unmap", "unmap [--no-regroup]", unmap_pass_run, NULL, false, ODIN3_PASS_PROCESSES_REFUSE};`. `unmap.c`: `um_run` per the interface; the module loop uses `odin3_module_get`. Add the PASSES.md row (columns as the `raise` row: pass and options, phase `7 (7.7)`, view `any view (no processes) -> same (hard cells become word cells)`, files, tests `test_unmap`, `cli_unmap_rtl`, `cli_unmap_blif`, one-line description pointing at the spec).
- [ ] **Step 4: Run:** `cmake --build --preset debug && ctest --preset debug -R "test_unmap|pass_manager" --output-on-failure` -> PASS. If a test counts or lists the built-in passes, update its expectation by one.
- [ ] **Step 5: Commit** `feat(unmap): pass scaffold and options` after `tools/lint.sh`.

### Task 2: Net classes, observed, type lookup, constants (N-1 to N-3, E-2, E-3)

**Files:**
- Create: `src/passes/unmap/kit.c`, `src/passes/unmap/kit.h`
- Modify: `src/passes/unmap/unmap.h` (the `um_ctx` struct), `src/passes/unmap/unmap.c`, `CMakeLists.txt` (add `kit.c`), `tests/unit/test_unmap.c`

**Interfaces:**
- Produces (`kit.h`):
  ```c
  typedef enum um_cls { UM_NONE, UM_PAD, UM_GND, UM_VCC, UM_SIG } um_cls;
  um_cls um_net_class(const odin3_module *module, odin3_net_id net);      /* N-1 */
  bool um_observed(const odin3_module *module, odin3_net_id net);         /* N-2 */
  /* The net on pin `index` of `port` of `node`; invalid net when the pin is open or out of range. */
  odin3_net_id um_port_net(const odin3_module *module, odin3_node_id node, uint32_t port,
                           uint32_t index);
  typedef struct um_types {
      odin3_celltype_id adder, multiply, sp_ram, dp_ram;   /* 0 when not registered */
      odin3_celltype_id add, sub, mul, mem, not_, and_, mux, dff, konst0, konst1;
  } um_types;
  /* E-2 (shape of registered hard types) and E-3 (word and constant types present). */
  odin3_status um_types_find(const odin3_design *design, um_types *types);
  ```
  In `unmap.h`: `typedef struct um_ctx { odin3_pass_ctx *pass; odin3_design *design; odin3_module *module; const um_opts *opts; um_types types; um_stats stats; odin3_net_id konst[2]; } um_ctx;` and
  `odin3_status um_const_net(um_ctx *uc, bool one, odin3_net_id *out);` (lowest-ID live `$_CONST0_`/`$_CONST1_` node's net in `uc->module`, else a new constant node on a new net with `odin3_module_prov`; the logic of `scan_const`/`make_const` in `hm_kit.c` lines 478 to 520, without the transaction lists).

- [ ] **Step 1: Write failing tests.** `test_net_class`: read a BLIF with `.names gnd`, `.names vcc` + row `1`, `.names unconn` (no rows), a `.subckt adder` using them and a primary input; assert `um_net_class` gives `UM_GND` for `gnd`, `UM_VCC` for `vcc`, `UM_PAD` for `unconn` (not `UM_GND`: pad before gnd), `UM_SIG` for the input, `UM_NONE` for an open pin. `test_net_class_x`: a net driven by a `$_CONSTX_` node is `UM_PAD`. `test_observed`: an output net that feeds only a module output port is observed; a dangling net is not. `test_types_find`: with the library loaded `um_types_find` is `ODIN3_OK` and `types.adder.v != 0`; on a design without the library `types.adder.v == 0` and still `ODIN3_OK`; a design where a user module is named `adder` (granularity not HARD) gives `ODIN3_ERR_INVALID_ARG` and the log has `unmap: cell type 'adder' does not have the expected ports`. `test_const_net`: `um_const_net` twice returns the same net; a design with an existing `vcc` returns that net.
- [ ] **Step 2: Run:** fails (symbols missing).
- [ ] **Step 3: Implement** the functions above. The pad test for `unconn`: net name `unconn` (via `odin3_net_name` and the design strtab), one driver, driver type is the built-in `$sop` with parameter 0 (WIDTH) `== 0` and an empty cover (copy `odin3_abc_net_undefined` in `src/passes/abc/region.c` lines 156 to 170; do not include the ABC header). `um_types_find` copies the `k_shapes` table and checks of `hm_need` in `src/passes/partial_map/hm_kit.c` (lines 22 to 42 and `hm_need`); names `$_CONST0_`, `$_CONST1_`; word types by `$add $sub $mul $mem $not $and $mux $dff`. Messages exactly as E-2 and E-3. `um_module` first calls `um_types_find` (once per run, in `um_run`).
- [ ] **Step 4: Run:** `ctest --preset debug -R test_unmap --output-on-failure` PASS.
- [ ] **Step 5: Commit** `feat(unmap): net classes, type lookup and constants`.

### Task 3: Replacement transaction `um_replace` (S-5, P-1, P-2)

**Files:**
- Modify: `src/passes/unmap/kit.c`, `src/passes/unmap/kit.h`, `tests/unit/test_unmap.c`

**Interfaces:**
- Produces (`kit.h`):
  ```c
  typedef struct um_group { odin3_vec nodes; } um_group;      /* odin3_node_id, in insertion order */
  void um_group_init(um_group *group);
  void um_group_free(um_group *group);
  odin3_status um_group_add(um_group *group, odin3_node_id node);
  typedef struct um_new {
      odin3_celltype_id type;
      const odin3_value *params;  uint32_t n_params;
      const odin3_netvec *ports;                 /* one per port of the type */
      odin3_node_id namesake;                    /* name and attributes come from this old node */
      const char *rule;                          /* value of the `unmap` attribute */
      int64_t origin;                            /* value of `unmap_origin`, or 0 for none */
      int64_t w;                                 /* value of `unmap_w`, or 0 for none */
  } um_new;
  /* S-5 steps 1 to 5. *out is the new node. On failure the group is intact. */
  odin3_status um_replace(um_ctx *uc, const um_group *group, const um_new *req,
                          odin3_node_id *out);
  /* P-3: sets the string attribute `unmap_note` (or `unmap_cand`) of node to `text`. */
  odin3_status um_set_str(um_ctx *uc, odin3_node_id node, const char *key, const char *text);
  ```
- [ ] **Step 1: Write failing test** `test_replace_provenance_and_attrs`: read a BLIF with two `adder` cells (any wiring), set the attribute `foo`=`bar` and `hardmap`=`adder:head` on the first via `odin3_attr_set`, build a `$add` (A_WIDTH 1, B_WIDTH 1, Y_WIDTH 1, params `{0,0,1,1,1}`) on the existing nets with a hand-written `um_new`, call `um_replace` with a group of both adders. Assert: both adders are not live; the new node's name equals the first adder's old name; attribute `foo` copied, `hardmap` absent, `unmap`=`chain`; `odin3_prov_get(design, odin3_node_prov(...))->kind == ODIN3_PROV_DERIVED` and its `parents` are the two adders' old records in node-ID order (a repeated record once); `check_clean()`. `test_replace_no_prov`: nodes with prov 0 give prov 0. `test_replace_failure_keeps_group`: a `um_new` with a wrong port count makes `odin3_node_create_connected` fail; assert the status is not `ODIN3_OK` and both adders are still live.
- [ ] **Step 2: Run:** fails.
- [ ] **Step 3: Implement** `um_replace`: (1) collect `odin3_node_prov` of every group node into an `odin3_vec`; `odin3_prov_begin_op(uc->pass)` and `odin3_prov_derive` only when at least one is non-zero (the shape of `make_prov` in `raise.c` lines 189 to 211); (2) `odin3_node_create_connected(uc->module, &(odin3_node_spec){type, 0, prov, params, n_params}, ports, &node)`; (3) `odin3_kit_copy_attrs(uc->module, namesake, node)` (`src/passes/opt/kit.h` line 191), `odin3_attr_remove` of `hardmap` and `partial_map_nocarry`, then attributes `unmap` (string), `unmap_pieces` (integer, `group->nodes.len`), `unmap_origin`, `unmap_w` when non-zero; (4) `odin3_node_delete` for each group node (when the first delete fails, delete the new node and return the status); (5) read the namesake's name before step 4, `odin3_node_rename(uc->module, node, name)` after it when the name is non-zero. Interning strings: `odin3_design_intern(design, odin3_bytes_cstr(s), &id)`; string attribute value `{ODIN3_VAL_STRING, 0, NULL, 0, id, 0}` as `set_hardmap_attr` in `hm_kit.c` line 260.
- [ ] **Step 4: Run:** PASS.
- [ ] **Step 5: Commit** `feat(unmap): replacement transaction`.

### Task 4: Carry chain discovery (C-1)

**Files:**
- Create: `src/passes/unmap/chain.c`, `src/passes/unmap/chain.h`
- Modify: `CMakeLists.txt`, `src/passes/unmap/unmap.c` (call `um_adder_phase` from `um_module` when `types.adder.v != 0`), `tests/unit/test_unmap.c`

**Interfaces:**
- Produces (`chain.h`):
  ```c
  /* One walk: cells in carry order, and why it cannot be recovered (NULL when it can). */
  typedef struct um_walk { odin3_vec cells; /* odin3_node_id */ const char *decline; } um_walk;
  /* C-1: the walks of the module, in node-ID order of their first cell. `walks` is an odin3_vec of
   * um_walk; the caller frees each `cells` and the vec (um_walks_free). Linear, no recursion. */
  odin3_status um_chain_walks(um_ctx *uc, odin3_vec *walks);
  void um_walks_free(odin3_vec *walks);
  odin3_status um_adder_phase(um_ctx *uc);        /* walks, then Task 5's conversion */
  ```
- [ ] **Step 1: Write failing tests** (BLIF with `.subckt adder` lines, library loaded; adder pins `a b cin cout sumout`): `test_walks_simple`: a head (`a=vcc b=gnd cin=gnd`) then 3 stages: `um_chain_walks` returns one walk of 4 cells in carry order, `decline == NULL`. `test_walks_order_independent`: the same chain with the `.subckt` lines in reverse order gives the same cell order. `test_walks_branch`: one `cout` feeding two `cin` pins: two walks (the first ends at the branching cell, the second starts at each successor). `test_walks_tap`: a `cout` feeding one `cin` and a LUT: the walk ends at that cell. `test_walks_cycle`: three adders in a ring: one walk with `decline` == `"cycle"` holding the three cells. Two unrelated chains give two walks in node-ID order of their first cell.
- [ ] **Step 2: Run:** fails.
- [ ] **Step 3: Implement** `um_chain_walks`: vectors `succ` and `pred` (node ID per node, 0 none) sized `odin3_module_node_end(uc->module)`; one pass over live nodes of type `types.adder` computing `cins` and `others` of the `cout` net (`odin3_net_sinks`, `odin3_pin_node`, `odin3_pin_port`, with the `cin` port index 2 and `cout` index 3 of `k_shapes`); link rule C-1; second pass: for each live adder without `pred`, walk `succ` in a `while` loop appending to a new `um_walk`; mark visited in a `uint8_t` vec; third pass: every unvisited live adder starts a cycle walk (walk `succ` until it returns to the start) with `decline = "cycle"`.
- [ ] **Step 4: Run:** PASS.
- [ ] **Step 5: Commit** `feat(unmap): carry chain discovery`.

### Task 5: Chains to `$add` / `$sub` (C-2 to C-8)

**Files:**
- Modify: `src/passes/unmap/chain.c`, `src/passes/unmap/chain.h`, `tests/unit/test_unmap.c`
- Create: `src/passes/unmap/operand.c`, `src/passes/unmap/operand.h` (shared with Task 6)

**Interfaces:**
- Produces (`operand.h`):
  ```c
  /* Bits of one operand as nets, LSB first; len entries. */
  typedef struct um_vec { odin3_net_id *nets; uint32_t len; } um_vec;
  /* C-5/N-3: shortens `v` while its top bit has class UM_GND or UM_PAD (and UM_NONE when
   * `open_too`), keeping at least one bit. Returns how many bits were removed. */
  uint32_t um_trim_tail(const odin3_module *module, um_vec *v, bool open_too);
  /* C-6: the top run: minimal w <= len with bits w-1..len-1 the same UM_SIG net; len when none. */
  uint32_t um_top_run(const odin3_module *module, const um_vec *v);
  ```
- Consumes: Task 4 (`um_walk`), Task 3 (`um_replace`, `um_set_str`), Task 2.
- [ ] **Step 1: Write failing tests** (in-session partial_map where noted; script lines run with `odin3_pass_run`, a Verilog file written to the temp dir like `read_blif` does; Verilog `module t(input [7:0] a, input [5:0] b, output [8:0] y); assign y = a + b; endmodule`, script `read_techlib LIB; read_verilog --vtr-compat F; proc; opt; hardmap; unmap; opt_clean`):
  - `test_unmap_add_roundtrip`: after the script there is exactly one `$add` node with A_WIDTH 8, B_WIDTH 6, Y_WIDTH 9, A_SIGNED 0, B_SIGNED 0, no live `adder` node, no node with attribute `hardmap`; the `$add` has attribute `unmap`=`chain` and `unmap_pieces`=10 (head + 8 stages + top... count the cells `hardmap` made: `n + 2` = 10); `check_clean()`; the log has the INFO line `unmap: 10 adder cells to 1 $add + 0 $sub` as a prefix of the run line (P-4; Task 11 finishes the line).
  - `test_unmap_sub_roundtrip`: `assign y = a - b;` (unsigned 8, 6): one `$sub` A_WIDTH 8 B_WIDTH 6; the shared `$not` cells are gone (live `$not` count 0).
  - `test_unmap_add_signed`: `input signed [7:0] a, input signed [3:0] b, output signed [8:0] y`: one `$add`, A_SIGNED 1, B_SIGNED 1, A_WIDTH 8, B_WIDTH 4 (C-6).
  - `test_unmap_not_shared`: hand BLIF: a `$sub`-shaped chain whose `$not` output also feeds a LUT: the `$sub` is made and the `$not` node is still live.
  - Review Focus 1: `test_unmap_cout_tap`: hand BLIF chain of head + 3 stages whose last `cout` also feeds a LUT: one `$add` with Y_WIDTH 4 and `Y[3]` on the `cout` net; with the tap in the middle: the first part recovered, the second part cells keep `unmap_note`=`adder:cin_carry` and stay `adder`.
  - `test_unmap_parmys_blif`: read `tests/golden/blif-vtr/adder_hard_block.parmys.vtr.blif` with the library, `unmap`: two `$add` cells, their stage operands with `unconn` bits trimmed, all `adder` gone, `check_clean()`.
  - `test_unmap_declines`: first stage with `cin` a primary input: note `adder:cin_signal`, cells unchanged; lone head: `adder:lone_head`; `cin` on `unconn`: `adder:cin_x`.
  - `test_trim_and_run`: unit tests of `um_trim_tail` and `um_top_run` on hand `um_vec`s (hand-built nets in a tiny BLIF).
- [ ] **Step 2: Run:** fails.
- [ ] **Step 3: Implement** `um_adder_phase`: `um_chain_walks`; per walk without `decline`: `static odin3_status convert_walk(um_ctx*, const um_walk*)` doing C-2 (head test; `majority` of the constant classes), C-3 (c0 from `cin` class; decline reasons `cin_x`, `cin_open`, `cin_carry`, `cin_signal`; a `cin_carry` is a `cin` net whose driver pin is the `cout` of a live `adder`), C-4 (k, drop stages, extra `Y` bit from an observed `cout`), C-5 (operand vectors; `$sub` inversion through `um_net_class` and a `$not` driver test: driver node type equals `types.not_` and its `A` port is 1 bit; absorb the `$not` into the group when every sink of its output net is a `b` pin of this walk), trim with `um_trim_tail(..., true)`, C-6 sign detection, the `um_new` with params `{A_SIGNED, B_SIGNED, A_WIDTH, B_WIDTH, Y_WIDTH}` (order of `src/ir/cells/cells.h` line 90) and ports `{A, B, Y}` where `Y` nets are the stages' `sumout` nets (+ the exposed `cout` net). Namesake `cells[0]`; `rule` `"chain"`. Split into helpers so each function is at most 60 lines: `chain_shape` (head, c0), `chain_stages` (k), `chain_operands` (A, B, sub inversion), `chain_sign` (C-6), `chain_emit` (um_new and um_replace). A declined walk or a walk with `decline` calls `um_set_str(uc, node, "unmap_note", "adder:<reason>")` on each cell and one WARNING per walk (P-3 text). Update `uc->stats` (`adder_cells`, `add`, `sub`, `declined`).
- [ ] **Step 4: Run:** PASS; also `ctest --preset debug -R "hardmap|pm_"` still PASS (partial_map untouched).
- [ ] **Step 5: Commit** `feat(unmap): adder chains to $add and $sub`.

### Task 6: `multiply` to `$mul` (M-1 to M-3)

**Files:**
- Create: `src/passes/unmap/mul.c`, `src/passes/unmap/mul.h`
- Modify: `src/passes/unmap/unmap.c` (call `um_mul_phase` when `types.multiply.v != 0`, after the adder phase), `CMakeLists.txt`, `tests/unit/test_unmap.c`

**Interfaces:**
- Produces: `odin3_status um_mul_phase(um_ctx *uc);` in `mul.h`. Each new `$mul` has `unmap`=`mul`, `unmap_w` = the source `WIDTH`.
- Consumes: Tasks 2, 3, 5 (`um_trim_tail`, `um_top_run`).
- [ ] **Step 1: Write failing tests.** `test_unmap_mul_8x7`: Verilog `assign p = a * b;` (8 x 7 inputs, 15 output bits), script as Task 5: one `$mul` A_WIDTH 8 B_WIDTH 7 Y_WIDTH 15, unsigned, `unmap_w` 36 (`lib/vtr.o3lib` default width; the `multiply` cell `WIDTH` is 36 on the k6 rules), no `multiply` live. `test_unmap_mul_signed`: `input signed [7:0] a, b; output signed [15:0] p` with `$signed` product: one `$mul`, A_SIGNED B_SIGNED 1, A_WIDTH 8, B_WIDTH 8, Y_WIDTH 16 (M-2). `test_unmap_mul_trunc`: unsigned 8x8 with 8-bit result (low half): `$mul` A 8 B 8 Y 8 unsigned. `test_unmap_mul_dead` (Review Focus 2): hand BLIF `multiply` with every `a`/`b` pin on `unconn` and no reader of `out`: one `$mul` with A_WIDTH 1, B_WIDTH 1, Y_WIDTH 2, `check_clean()`. `test_unmap_mul_blif`: `tests/golden/blif-vtr/multiply_narrow.vtr.blif`: one `$mul` A_WIDTH 8 B_WIDTH 8 Y_WIDTH 16.
- [ ] **Step 2: Run:** fails.
- [ ] **Step 3: Implement** `um_mul_phase` per M-1/M-2: operand vectors from `um_port_net` over the `a`/`b` ports (indices 0 and 1 of `multiply`), `ua`/`ub` by scanning for the last pin whose class is not `UM_PAD`/`UM_NONE`; `y` by the last observed `out` pin; signed test of M-2 with `um_top_run`; params `{A_SIGNED, B_SIGNED, A_WIDTH, B_WIDTH, Y_WIDTH}`; ports `{A, B, Y}` with `Y[k]` = `out[k]` net. A `multiply` whose `WIDTH` parameter is missing or less than 1 is declined `mul:width`. Stats `multiply`, `mul`.
- [ ] **Step 4: Run:** PASS.
- [ ] **Step 5: Commit** `feat(unmap): multiply to $mul`.

### Task 7: Multiplier regroup, pattern P1 (M-4, M-5)

**Files:**
- Create: `src/passes/unmap/mul_group.c`, `src/passes/unmap/mul_group.h`
- Modify: `src/passes/unmap/unmap.c` (call `um_mul_group_phase` when `uc->opts->regroup`, after `um_mul_phase`), `CMakeLists.txt`, `tests/unit/test_unmap.c`

**Interfaces:**
- Produces: `odin3_status um_mul_group_phase(um_ctx *uc);` (scan loop of M-5) and, for Task 8, `static bool match_p2(...)` declared in the same file; header exports only the phase function.
  Internal helper signatures (file `mul_group.c`):
  ```c
  typedef struct um_piece { odin3_node_id node; um_vec a, b, y; uint32_t w; } um_piece;
  static bool load_mul(um_ctx *uc, odin3_node_id node, um_piece *piece);   /* unsigned $mul with unmap_w */
  static bool sole_sink(const um_ctx *uc, odin3_net_id net, odin3_node_id reader); /* step 4 */
  static odin3_status try_p1(um_ctx *uc, odin3_node_id root, bool *merged);
  ```
- [ ] **Step 1: Write failing tests** (script as Task 5, in-session; arithmetic shapes of spec section 1): `test_unmap_mul_split_a`: `input [39:0] a, input [7:0] b, output [47:0] p; assign p = a * b;` -> after `hardmap; unmap; opt_clean` one `$mul` A_WIDTH 40, B_WIDTH 8, Y_WIDTH 48, exactly one `$mul`, no `$add`, no `multiply`/`adder` live, `unmap`=`mul.p1`, `unmap_w` 36, `unmap_pieces` = number of cells removed. `test_unmap_mul_split_b`: `a` 8 bits, `b` 40 bits: one `$mul` A_WIDTH 8, B_WIDTH 40. `test_unmap_mul_no_regroup`: the same with `unmap --no-regroup`: two `$mul` and one `$add` remain (counted by node type name). `test_unmap_mul_split_other_w`: hand BLIF with two `multiply` cells and an `adder` chain wired like P1 but with split sizes 5 and 6 (neither equals the cell `WIDTH` 36): not merged, two `$mul` and a `$add` remain, no `unmap_note`.
- [ ] **Step 2: Run:** fails.
- [ ] **Step 3: Implement** the scan loop: collect live `$add` nodes with attribute `unmap`=`chain`/`mul.*` in node-ID order; `try_p1` implements the numbered steps 1 to 5 of M-4 P1 including the swapped choice of LO/HI and of shared port; the merged cell via `um_replace` (group = LO, HI, R; namesake LO; `rule` `"mul.p1"`; `w` = W). Repeat the scan while any `merged` (cap the loop at the number of live `$add` nodes plus one, a safety net; exceeding it is `ODIN3_ERR_INVALID_ARG` with the message `unmap: multiplier regroup did not converge`). Stats `regrouped_mul`.
- [ ] **Step 4: Run:** PASS.
- [ ] **Step 5: Commit** `feat(unmap): regroup one-side split multipliers`.

### Task 8: Multiplier regroup, pattern P2 and nesting (M-4 P2, M-5, M-6)

**Files:**
- Modify: `src/passes/unmap/mul_group.c`, `tests/unit/test_unmap.c`

**Interfaces:**
- Produces (in `mul_group.c`): `static odin3_status try_p2(um_ctx *uc, odin3_node_id root, bool *merged);` tried before `try_p1` in the scan.
- [ ] **Step 1: Write failing tests:** `test_unmap_mul_split_ab`: 40 x 40, `p` 80 bits: one `$mul` A_WIDTH 40, B_WIDTH 40, Y_WIDTH 80, `unmap`=`mul.p2`; no `$add`. `test_unmap_mul_split_nested`: 80 x 80 (160-bit result): one `$mul` 80 x 80 (needs two scans; assert `um_stats.regrouped_mul` through the INFO line `... 9 multiply to 1 $mul (...)`: 9 hard multipliers, 1 `$mul`). `test_unmap_mul_split_80x40`. `test_unmap_mul_low_only`: 40 x 40 with a 32-bit result: one `$mul` A_WIDTH 36, B_WIDTH 36, Y_WIDTH 32 (M-7: widths are `W`; add the line to the expectation comment). Review Focus 3: `test_unmap_mul_shared_product`: 40 x 8 with the design also reading the raw low product of the first piece in a second output (hand BLIF edit of the hard-cell netlist: a LUT reading `m00.out[40]`'s net, i.e. a net on the adder's input side): no merge, both reading cells still driven, the root `$add` has `unmap_note`=`mul:shared`, `check_clean()`.
- [ ] **Step 2: Run:** fails.
- [ ] **Step 3: Implement** `try_p2` per P2 steps 1 to 4 (drivers by `odin3_net_driver` + `odin3_pin_node`; the cross-cell assignment test of step 2 in both orders); merged `$mul` params as in M-4; group `M00 M11 C1 C2 R0 R1`; `rule` `"mul.p2"`. On a failed sink test only (every other condition held) set note `mul:shared` on the root `$add`.
- [ ] **Step 4: Run:** PASS (`ctest --preset debug -R test_unmap`).
- [ ] **Step 5: Commit** `feat(unmap): regroup both-side split multipliers`.

### Task 9: RAM cells to `$mem` (R-1 to R-4)

**Files:**
- Create: `src/passes/unmap/ram.c`, `src/passes/unmap/ram.h`
- Modify: `src/passes/unmap/unmap.c` (call `um_ram_phase` when a RAM type is registered), `CMakeLists.txt`, `tests/unit/test_unmap.c`

**Interfaces:**
- Produces (`ram.h`):
  ```c
  /* The summary of one hard RAM cell (R-2). */
  typedef struct um_ram {
      odin3_node_id node;
      bool dual;
      uint32_t abits, width, rd_ports, wr_ports;
      bool read[2], write[2];
  } um_ram;
  odin3_status um_ram_summary(const um_ctx *uc, odin3_node_id node, um_ram *out);   /* R-2 */
  /* Builds the $mem for the cells of `members` (um_ram, one or more) and replaces them. */
  odin3_status um_ram_emit(um_ctx *uc, const odin3_vec *members, const char *rule);
  odin3_status um_ram_phase(um_ctx *uc);        /* Task 9: every cell alone; Task 10 groups first */
  ```
  Pin indices come from the `ram_layout` table of `src/passes/partial_map/ram.h` (single: clk 0, data 1, addr 2, we 3, out 4; dual: clk 0, data2 1, data1 2, addr2 3, addr1 4, we2 5, we1 6, out2 7, out1 8, port 1 = the `*1` pins, port 2 = the `*2` pins); copy the table as a `static const` in `ram.c` with a comment naming `src/passes/partial_map/ram.c` `k_sp_layout`/`k_dp_layout`.
- [ ] **Step 1: Write failing tests** (script as Task 5; designs from `tests/micro/verilog/common/spram.v`, `dpram.v`, `rom.v`, `mem.v`; use `tests/micro_pm`/`tests/hardmap` inputs where a fixture exists): `test_unmap_spram`: `read_techlib LIB; read_verilog --vtr-compat tests/micro/verilog/common/spram.v; proc; opt; hardmap; unmap; opt_clean` -> one `$mem` node (and none of `single_port_ram`); its params: `ABITS` = the source address bits, `WIDTH` = the source data bits, `RD_PORTS` 1, `WR_PORTS` 1, `RD_CLK_ENABLE` bit 1, `RD_CLK_POLARITY` 1, `RD_TRANSPARENT` 0, `WR_CLK_ENABLE` 1, `INIT` len 0, `SIZE` = `2^ABITS`; `RD_EN` driven by a `vcc` net; `WR_EN` all bits on the one `we` net. `test_unmap_dpram`: RD_PORTS 2, WR_PORTS 2. `test_unmap_rom` (`rom.v`): WR_PORTS 0, RD_PORTS 1, no `WR_EN` pins. `test_unmap_ram_dead` (R-3): a hand BLIF `single_port_ram` with nothing read and `we=gnd`: one `$mem` WIDTH 1, ABITS 1, RD_PORTS 1, WR_PORTS 0. `test_unmap_ram_blif_pad`: a hand BLIF `single_port_ram ADDR_WIDTH=15 DATA_WIDTH=1` with `addr[6..14]` on `unconn`: `$mem` ABITS 6. Every case ends with `check_clean()`.
- [ ] **Step 2: Run:** fails.
- [ ] **Step 3: Implement** `um_ram_summary` (R-2: `has_write` from the class of the `we` net being not `UM_GND`; `has_read` from any observed `out` pin; `abits`, `width`, counts), `um_ram_emit` (parameter array of `$mem` in the order of `src/ir/cells/mem.c` lines 5 to 18: `MEMID` string (the namesake's name text, or `$mem$<node id>` when unnamed), `SIZE`, `OFFSET`, `ABITS`, `WIDTH`, `RD_PORTS`, `WR_PORTS`, bits for the four clock parameters and `RD_TRANSPARENT` (`ODIN3_VAL_BITS` values: one `odin3_bit` byte per port, LSB first; `ODIN3_BIT_1`/`ODIN3_BIT_0`), `INIT` BITS with `len 0`); the `$mem` ports are port-major (`RD_ADDR` index `port*ABITS + i`, `RD_DATA`/`WR_DATA`/`WR_EN` index `port*WIDTH + j`); missing address bits and write data bits beyond the used width use `um_const_net(false)` (address) and the source net (data inside the width), `RD_EN` = `um_const_net(true)`. `um_ram_phase` calls `um_ram_summary` then `um_ram_emit` for each cell, node-ID order; `rule` `"ram"`; stats `ram_cells`, `mem`.
- [ ] **Step 4: Run:** PASS.
- [ ] **Step 5: Commit** `feat(unmap): RAM cells to $mem`.

### Task 10: RAM grouping by origin (R-5 to R-7)

**Files:**
- Modify: `src/passes/unmap/ram.c`, `src/passes/unmap/ram.h`, `tests/unit/test_unmap.c`

**Interfaces:**
- Produces (`ram.h`): `odin3_status um_ram_groups(um_ctx *uc, const odin3_vec *cells, odin3_vec *groups);` where `cells` holds `um_ram` and `groups` becomes an `odin3_vec` of `odin3_vec` (each a list of `um_ram` indices into `cells`, ascending); `um_ram_phase` is changed to: summaries, `um_ram_groups` when `uc->opts->regroup`, then `um_ram_emit` per group; groups of one use rule `"ram"`, width groups `"ram.width"`, replica groups `"ram.replica"`. Candidate sets get `um_set_str(..., "unmap_cand", "ram:<N>")`.
- [ ] **Step 1: Write failing tests.** `test_unmap_ram_width_merge`: `tests/micro/verilog/common/spram.v` widened or a new fixture `tests/unmap/ram_w8.v` (8-bit by 64-word memory, synchronous read and write): after `hardmap` there are 8 RAM slices; after `unmap` one `$mem`, WIDTH 8, ABITS 6, `unmap`=`ram.width`, `unmap_pieces` 8, `unmap_origin` non-zero and equal to the record of the first slice's old node (record read before `unmap` with `odin3_node_prov`), data bit `j` of `WR_DATA` on the net that fed slice `j` (compare against the RAM source Verilog input net order through the net names `d[j]`). `test_unmap_ram_bit_order`: node-ID order equals the `hardmap` role order `b0..b7` (read the attribute in the test, the only place it is read). `test_unmap_ram_replicas`: `tests/micro/verilog/common/2r1w.v`-like (one write, two reads on different addresses): after `hardmap` two RAM cells, after `unmap` one `$mem` with RD_PORTS 2, WR_PORTS 1, `unmap`=`ram.replica`. `test_unmap_ram_blif_candidates` (Review Focus 4): hand BLIF with two `single_port_ram` cells (same `clk`, `addr`, `we`, different `data` and `out`): not merged, two `$mem`, both with `unmap_cand`=`ram:0`; INFO line `unmap: 2 RAM cells with equal control nets and different origin kept apart (candidate set ram:0)`. `test_unmap_ram_no_regroup`: the 8 slices with `--no-regroup`: 8 `$mem`.
- [ ] **Step 2: Run:** fails.
- [ ] **Step 3: Implement** `um_ram_groups`: key per summary = (origin record, `dual`, `abits`, `width`, read/write flags, the nets of `clk`, `addr`, `we` pins per port); sort indices by key with a stable insertion into an `odin3_vec` of group heads using a linear scan over groups (groups are few; use `odin3_u64map` keyed by a hash of the key only if a profile shows need, otherwise O(groups) per cell is acceptable up to the sweep sizes: add a note in the code). Width groups first (same key except data/out nets), then replica groups among the width groups (same origin and write-pin nets). Origin 0 or different: candidate sets as R-6. Bit order by node ID, with a debug-only (`assert`) comparison to the role numbers is NOT allowed (rules never read `hardmap`); the test above compares them.
- [ ] **Step 4: Run:** PASS.
- [ ] **Step 5: Commit** `feat(unmap): RAM width and replica merge by origin`.

### Task 11: Diagnostics, idempotence, robustness (P-3, P-4, S-6, S-7)

**Files:**
- Modify: `src/passes/unmap/unmap.c`, `src/passes/unmap/register.c`, `tests/unit/test_unmap.c`

**Interfaces:**
- Produces: the INFO line of P-4 from `um_run` (one per run, only when at least one hard cell was seen; else nothing is logged): format `unmap: %llu adder cells to %llu $add + %llu $sub, %llu multiply to %llu $mul (%llu regrouped), %llu RAM cells to %llu $mem, %llu groups left (see unmap_note)`.
- [ ] **Step 1: Write failing tests.** `test_unmap_info_line`: after the Task 5 script the log contains `unmap: 10 adder cells to 1 $add + 0 $sub, 0 multiply to 0 $mul (0 regrouped), 0 RAM cells to 0 $mem, 0 groups left (see unmap_note)`. Review Focus 5: `test_unmap_idempotent`: run `unmap` twice on the 40x40 multiplier: after the second run `write_rtlil` text is byte-identical to after the first (write both to the temp dir and compare), and no log line of the second run counts recovered cells. `test_unmap_no_lib`: a design read from Verilog with no library loaded: `unmap` is `ODIN3_OK` and the log has no unmap line. `test_unmap_two_modules`: a BLIF with two models (each one chain) and `--top` unset: both recovered. `test_unmap_determinism`: two sessions on the same input give equal `write_rtlil`.
- [ ] **Step 2: Run:** fails.
- [ ] **Step 3: Implement** the line (`odin3_log(ODIN3_LOG_INFO, ...)`), the per-group WARNING texts of P-3 through a helper `um_warn_group(const um_ctx*, odin3_node_id, const char *kind, const char *reason)` in `kit.c` located by the cell's provenance: copy `hm_diag`/`first_leaf` of `src/passes/partial_map/hm_kit.c` lines 89 to 140 (first source leaf of the provenance, else bare text, prefix `unmap: `).
- [ ] **Step 4: Run:** `ctest --preset debug -R test_unmap` PASS.
- [ ] **Step 5: Commit** `feat(unmap): log line, warnings, idempotence`.

### Task 12: CLI round trip on the micros, goldens (T-2, T-3, T-8)

**Files:**
- Create: `tests/unmap/micros.txt`, `tests/unmap/expect.txt`, `tests/unmap/README.md`, `tests/unmap/*.v` (new micros: `add_signed.v`, `sub_signed.v`, `mul_40x40.v`, `mul_40x8.v`, `mul_8x40.v`, `mul_80x80.v`, `mul_signed.v`, `ram_w8.v`), `tests/unmap/expected/{add_8x6,sub_8x6,mul_8x7,mul_40x40,spram}.il`
- Modify: `tests/cli/test_cli.sh` (cases `unmap_rtl`, `unmap_golden`), `CMakeLists.txt` (tests `cli_unmap_rtl`, `cli_unmap_golden`, label `unit`; copy the `cli_hardmap_mul` stanza at line 1004)

**Interfaces:**
- Consumes: the script `tools/hardmap-check/phase3_pre_abc.script` (placeholders `@LIB@ @ADDERLIB@ @DESIGN@ @OUT@`, read the file and how `tools/hardmap-check/hardmap-diff` substitutes them) and `tools/equiv-check/equiv-check`. Format of `micros.txt`: one `path top` per line like `tests/partial_map/identity_inputs.txt`; `expect.txt`: `<path> <reason>` per line, a line that does not occur fails (the convention of `tests/hardmap/diff_expect.txt`).
- [ ] **Step 1: Write the failing case.** In `test_cli.sh`: `unmap_rtl)` runs, per micro, two scripts through `"$odin3" -p`: the Phase 3 script with `hardmap` replaced by `hardmap; unmap; opt_clean` -> `new.blif`, and the script with `hardmap` removed -> `ref.blif`; then `"$equiv" ref.blif new.blif`: exit 0, or exit 4 only when `expect.txt` lists the micro with `unproven`; also counts `$add $sub $mul $mem` before `lower` equal between a run that stops after `opt -full` and one after `unmap` via `write_rtlil` + `grep -c '^ *cell \\$add'` (and the others), except entries `counts` in `expect.txt` (the 40x40 family and `low_only` cases list their `M-7` reason). `unmap_golden)` runs the micros of T-3 through `read_techlib; read_verilog --vtr-compat; proc; opt; hardmap; unmap; opt_clean; write_rtlil` and `diff`s with `tests/unmap/expected/<name>.il` (header lines starting `#` ignored as in the other cases). Micros: the 26 of `tests/hardmap/micros.txt` that contain `$add`, `$sub`, `$mul`, `$mem` (reuse the list file path directly, filter by the script: any that ends with no change is allowed) plus the new ones.
- [ ] **Step 2: Run:** `ctest --preset debug -R "cli_unmap" --output-on-failure` fails (cases missing or goldens missing).
- [ ] **Step 3: Implement the fixtures.** Write the new micros (small Verilog, one module each); generate the goldens once with the commands of the golden case and review them by eye (each file is a few dozen lines: one `$add` etc.); if `write_rtlil` refuses a design (log shows a located refusal), drop that micro from the golden list and keep it in the round trip only; note the reason in `tests/unmap/README.md`. Fill `expect.txt` only with entries that actually occur.
- [ ] **Step 4: Run:** both cases PASS; `ctest --preset debug -R "hardmap|cli_pm" ` still PASS.
- [ ] **Step 5: Commit** `test(unmap): RTL round trip and goldens`.

### Task 13: BLIF without provenance, ambiguity micros (T-4, T-5, T-6)

**Files:**
- Create: `tests/unmap/blif/branch.blif`, `tap.blif`, `ram_pair_equal_ctl.blif`, `README` lines in `tests/unmap/README.md`
- Modify: `tests/cli/test_cli.sh` (case `unmap_blif`), `CMakeLists.txt` (`cli_unmap_blif`), `tests/unmap/expect.txt`

- [ ] **Step 1: Write the failing case.** `unmap_blif)`: for X in `tests/golden/blif-vtr/adder_hard_block.parmys.vtr.blif` and `multiply_narrow.vtr.blif` (and, when `~/odin3-ws/golden/k6_frac_N10_frac_chain_mem32K_40nm` exists, the Parmys BLIF of the `common/` micros `adder mult mem rom spram dpram`; otherwise skip those with exit 77 like `hardmap_diff_micro`): (a) T-4 `read_techlib LIB; read_blif X; unmap; opt_clean; hardmap; compact; write_blif --vtr out`; `equiv-check X out` exit 0 or 4 listed in `expect.txt`; per-model hard counts (grep `.subckt adder|multiply|single_port_ram|dual_port_ram` counts) of X and `out` equal. (b) T-5 for chain-only BLIFs: `read_blif X; unmap; lower; abc --lut 4; write_blif out2`; `equiv-check X out2` exit 0. (c) T-6: the three ambiguity BLIFs: after `unmap`, `write_rtlil` (or a `dump_attrs` grep via `write_json --odin3`) shows `unmap_note` `adder:cin_carry` for `branch`/`tap`, and for `ram_pair_equal_ctl` `unmap_cand` on both and two `$mem`; the log contains the candidate INFO line.
- [ ] **Step 2: Run:** `ctest --preset debug -R cli_unmap_blif --output-on-failure` fails.
- [ ] **Step 3: Write the BLIF fixtures** (hand-made, each under 40 lines, `.model m`, `.subckt` lines as in `adder_hard_block.parmys.vtr.blif`, the vtr models appended with `.blackbox` stanzas as that file does so `read_blif` works without a library when needed; the cases above use the library anyway). Debug any real failure in the rules, with a new unit test for the cause (Review Focus: `unconn` operands, dummy-output heads).
- [ ] **Step 4: Run:** PASS.
- [ ] **Step 5: Commit** `test(unmap): VTR BLIF round trip and ambiguity cases`.

### Task 14: VTR sweep tool (T-7)

**Files:**
- Create: `tools/unmap-check/unmap-vtr` (executable Python 3, stdlib only, `ruff` and `mypy --strict` clean), `tests/tools/test_unmap_vtr.py` (unittest), `tests/unmap/vtr_expect.txt`
- Modify: `CMakeLists.txt` (test `unmap_vtr_sweep` with `odin3_add_sweep_test`, label `sweep`, copied from `hardmap_vtr_sweep` near line 4225; plus a CTest for the Python unittest)

**Interfaces:**
- CLI: `unmap-vtr --odin3 BIN --arch XML [--expect FILE] DESIGN.v...`: per design runs the Phase 3 script with `hardmap; unmap` (output to a scratch BLIF is not needed), parses the INFO line and `unmap_note`/`unmap_cand` counts from the log, prints one row per design (`name  hard_adder hard_mul hard_ram  add sub mul mem  declined cands`), exits 1 when a design fails or a decline reason is `cycle`/`cin_carry` and not in `--expect`, 2 on usage errors. Rows run in the order of `tests/hardmap/vtr_designs.txt` (smallest first); the run goes through `tools/heavy-run` like the other sweeps.
- [ ] **Step 1: Write the failing test** `tests/tools/test_unmap_vtr.py`: `test_parse_info_line` (given the P-4 text, returns the counts), `test_exit_codes` (a fake `odin3` shell script printing a canned log: exit 0, and exit 1 for a `cycle` note, 2 for no designs).
- [ ] **Step 2: Run:** `python3 -m unittest tests.tools.test_unmap_vtr` fails.
- [ ] **Step 3: Implement** the tool and the CMake wiring. Do not run the 19 designs in this task unless the lanes are idle (heavy-lock rule: a sweep stalls every lane's build); run the three smallest designs by hand to check the tool and record the rows in the commit message.
- [ ] **Step 4: Run:** the unittest PASS; `ruff check tools/unmap-check tests/tools/test_unmap_vtr.py && mypy --strict tools/unmap-check/unmap-vtr`.
- [ ] **Step 5: Commit** `feat(unmap): VTR sweep tool`.

### Task 15: Depth recovery, write side (R-8 steps 1 and 3 for writes)

**Files:**
- Create: `src/passes/unmap/ram_depth.c`, `src/passes/unmap/ram_depth.h`
- Modify: `src/passes/unmap/unmap.c` (call `um_ram_depth_phase` after `um_ram_phase` when `uc->opts->regroup`), `CMakeLists.txt`, `tests/unit/test_unmap.c`

**Interfaces:**
- Produces:
  ```c
  odin3_status um_ram_depth_phase(um_ctx *uc);
  /* R-8 step 1 for one candidate set of $mem nodes (same unmap_origin): fills `paths` with the path
   * bits of each leaf and `ad` with the k address nets; false when the structure does not match. */
  bool um_depth_write_tree(const um_ctx *uc, const odin3_vec *leaves, uint32_t k, odin3_vec *paths,
                           odin3_vec *ad, odin3_net_id *we0);
  ```
- [ ] **Step 1: Write failing tests** (in-session, RTLIL view, no `lower`). Fixture `tests/unmap/ram_deep.v`: a 1-bit by 2^17-word synchronous memory (17 address bits are above the k6 limit 15, so k = 2 and four leaves). Script `read_techlib LIB; read_verilog --vtr-compat F; proc; opt; hardmap; unmap --no-regroup` leaves four `$mem` leaves (each `unmap_origin` equal and non-zero) and the word-level glue; the test collects the four `$mem` nodes in node-ID order and calls `um_depth_write_tree` directly. `test_depth_write_tree`: returns true with `k = 2`, `paths` holding the four distinct values 0..3 (one per leaf), `ad` holding the nets of address bits 0 and 1 of the source (compare with the nets named `addr[0]`, `addr[1]` of the input ports) and `we0` the net of the input `we`. `test_depth_write_mismatch`: rewire one leaf's `$and` second input to another address net with `odin3_pin_disconnect` and `odin3_pin_connect`: returns false. `test_depth_write_duplicate_path`: rewire so two leaves share a path: false.
- [ ] **Step 2: Run:** fails.
- [ ] **Step 3: Implement** `um_depth_write_tree` per R-8 step 1: for each leaf `WR_EN[0]` net, walk driver pins upward in a loop: a node of type `types.and_` with two inputs `prev` and `t`; classify `t` (a `UM_SIG` net that is not the output of a `$not` is `ad[L]` with bit 1; the output of a `types.not_` node gives `ad[L]` = its input and bit 0); depth counted from the leaf; stop at depth k; all leaves must end at one `we0` and agree on `ad[L]` per level; verify that the set of paths has 2^k distinct elements (bitset in a `uint8_t` vec).
- [ ] **Step 4: Run:** PASS.
- [ ] **Step 5: Commit** `feat(unmap): depth recovery, write enable tree`.

### Task 16: Depth recovery, read side and merge (R-8 steps 2 to 4)

**Files:**
- Modify: `src/passes/unmap/ram_depth.c`, `tests/unit/test_unmap.c`

**Interfaces:**
- Produces: `bool um_depth_read_tree(const um_ctx *uc, const odin3_vec *leaves, const odin3_vec *paths, const odin3_vec *ad, odin3_vec *roots, odin3_vec *glue);` (roots: per read port and data bit the root net; glue: the `$mux` and `$dff` nodes to absorb) and the merge in `um_ram_depth_phase` (one `$mem`, `ABITS` = `k + leaf ABITS`, address nets `ad[0..k)` then the leaf's address nets, `WR_EN` = `we0` copies, `RD_DATA` = the roots, rule `"ram.depth"`, `unmap_origin` kept).
- [ ] **Step 1: Write failing tests.** `test_unmap_depth_roundtrip`: the Task 15 fixture with plain `unmap` (regroup on): after `hardmap; unmap; opt_clean` exactly one `$mem` with ABITS 17, WIDTH 1, RD_PORTS 1, WR_PORTS 1, `unmap`=`ram.depth`, and no live `$mux`, `$dff`, `$and` or `$not` node (the fixture has no user logic). `test_unmap_depth_shared_not`: add a user LUT-free `$not` of address bit 0 read by an extra output: the merge happens and that `$not` stays live. `test_unmap_depth_mismatch_note`: break the mux tree of one data bit (swap the `$mux` inputs of one node): no merge, four `$mem` remain, each with `unmap_note`=`ram:depth`, one WARNING. `test_unmap_depth_lowered`: `hardmap; lower; unmap` (netlist view, glue is LUTs): four `$mem`, no note, no warning. Equivalence evidence for the depth case is structural only (a 2^17-word memory is too large for `equiv-check`): `tests/unmap/expect.txt` lists `tests/unmap/ram_deep.v equiv-skip:too-large` and the `unmap_rtl` case counts `$mem` (1) and the absence of glue instead; the same smaller memory with a lowered depth limit is not available because the limit comes from the arch record.
- [ ] **Step 2: Run:** fails.
- [ ] **Step 3: Implement** the read side: for each read port, for each data bit, from the leaf `RD_DATA[j]` nets climb: the net's sink must be a `types.mux` node whose `A`/`B` input holds it; the node's `S` net is the output of a `types.dff` node whose `D` is `ad[L]` and whose `CLK` is the read clock; the level L is determined by the walk from the root (root level 0) so climb from the leaves checks depth k and the leaf order against `paths`; iterative with an explicit worklist (a vec of nodes per level), never recursive. Absorb the glue by S-5 rule 3 of R-8 (a net with sinks outside stays driven; the cell is then not deleted). Mismatch anywhere: `ram:depth` note on the leaves and one WARNING, no change.
- [ ] **Step 4: Run:** `ctest --preset debug -R "test_unmap|cli_unmap"` PASS.
- [ ] **Step 5: Commit** `feat(unmap): depth recovery, read side and merge`.

### Task 17: Docs, lint, final gates (T-9)

**Files:**
- Modify: `docs/PASSES.md` (final text of the row: options, phases, attributes `unmap`, `unmap_pieces`, `unmap_origin`, `unmap_w`, `unmap_note`, `unmap_cand`), `docs/PHASE7.md` (a checklist item `7.7 unmap` and a decisions-log entry marked agent default: the spec's open questions with their recommendations), `docs/user/` (the passes page if it lists passes: grep `raise` in `docs/user`; add a short `unmap` entry), `CHANGELOG.md` (one line under unreleased), `docs/RE-ARCHITECTURE.md` (one line: `unmap` gives the F1 floor its word-level cells)
- Test: whole suite

- [ ] **Step 1: Run the full gates:** `tools/lint.sh` (clang-tidy included), `cmake --build --preset debug && ctest --preset debug` and `cmake --preset release && cmake --build --preset release && ctest --preset release -L unit`. Expected: all PASS, no new warning.
- [ ] **Step 2: Fix anything red** at the source (never `NOLINT`); each fix with its unit test.
- [ ] **Step 3: Write the docs** above. `docs/PASSES.md` is the pass table; keep the row to the length of the `raise` row plus the attribute list.
- [ ] **Step 4: Verify** `grep -rn "unmap" docs/PASSES.md docs/PHASE7.md | head` shows the entries, `cmake --build --preset debug` passes, and `python3 tools/check_rules.py` (if wired by lint) is clean.
- [ ] **Step 5: Commit** `docs(unmap): pass table, phase log, user manual`; then `git push -u origin feat/unmap`. Open the PR only after a read-only critique agent has reviewed the branch (CLAUDE.md rule 12) and the VTR sweep rows of Task 14 are attached to the PR description.

---

## Self-review

- **Spec coverage:** S-1/S-4 Task 1; N-1 to N-3, E-2, E-3 Task 2; S-5, P-1, P-2 Task 3; C-1 Task 4; C-2 to C-8 Task 5; M-1 to M-3 Task 6; M-4 P1, M-5 Task 7; M-4 P2, M-6, M-7 Task 8; R-1 to R-4 Task 9; R-5 to R-7 Task 10; P-3, P-4, S-6, S-7, E-4 Task 11; T-1 Tasks 1 to 11, 15, 16; T-2, T-3, T-8 Task 12; T-4 to T-6 Task 13; T-7 Task 14; R-8 Tasks 15 and 16; T-9 Task 17; E-1 Task 1.
- **Names checked against the tree:** `odin3_pass_arg_next`, `odin3_pass_arg_is`, `odin3_node_create_connected`, `odin3_node_delete`, `odin3_node_rename`, `odin3_attr_set`, `odin3_attr_remove`, `odin3_kit_copy_attrs`, `odin3_prov_begin_op`, `odin3_prov_derive`, `odin3_net_sinks`, `odin3_net_driver`, `odin3_pin_node`, `odin3_pin_port`, `odin3_net_const_value`, `odin3_design_intern`, `odin3_module_node_end`, `odin3_design_module_end`, `odin3_celltype_get`, `odin3_node_param`, `odin3_node_port`, `odin3_pin_net`, `ODIN3_PASS_PROCESSES_REFUSE`.
- **Proportion:** the tests carry the decisions; bodies are left to the implementer except the algorithms the spec fixes (patterns P1/P2, chain rules, RAM mapping).
