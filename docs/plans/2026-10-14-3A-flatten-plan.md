# 3A `flatten` — Implementation Plan (v1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

v1 (2026-10-14), for Peter's approval (CLAUDE.md rule 2): **no code before he approves** this plan and
`docs/specs/2026-10-14-3A-flatten-design.md` (the spec; its FL-n and § numbers are binding here).

**Goal:** a pass `flatten` that expands every module instance of the design's top (and of every
`keep_hierarchy` module) into the module that holds it, with Yosys-style dotted instance-path names,
IR-2 aliases for the joined port nets, the instance path in `DERIVED` provenance records, and black
boxes, hard cells, library cells and `keep_hierarchy` instances left alone.

**Architecture:** `src/passes/flatten/` — a pre-flight on the module graph (cell-type → module map, Kahn
sort, cycle report, size estimate against the caps and the shared budget), then a top-down, in-place
expansion with a FIFO worklist (each flat object created once, from the original child), a net map
that joins port nets with `odin3_net_merge`/`odin3_wire_create` aliases, prefixed names with the
`_N` uniqueness rule, copied attributes, and one provenance operation per expanded instance. One
additive IR function, `odin3_prov_derive_hier`. Evidence tools in `tools/flatten-check/`.

**Tech stack:** C17, `src/util`, `src/ir` (+ `odin3_prov_derive_hier`), `src/passes/budget.[ch]`,
`src/sim` (as the oracle, not a dependency of the pass); Unity; Python 3 stdlib; Yosys 0.55
(`ODIN3_YOSYS055`, `$VTR_ROOT/build/bin/yosys`).

**Spec:** `docs/specs/2026-10-14-3A-flatten-design.md` v1.

Also: `docs/IR.md` (IR-2, IR-3, IR-4, IR-6, IR-7, IR-7b, IR-10, IR-12, IR-13, IR-14, IR-15, IR-16,
IR-19), `docs/DESIGN.md` §5.1, §6, §13 #6, PHASE3 (3A row, decisions 3, 7, 16, 17, 19, 20, 28, risks 8
and 9), the 2D spec (EL-4, EL-6, EL-9, EL-11, EL-21, EL-33, EL-45), the 3E spec (E10), the 2F spec/plan (the
format and `opt-diff`/`opt-equiv`), Yosys 0.55 `passes/techmap/flatten.cc`, and the standing rules
(heavy-run, no nightly, lint).

**Ordering and bases.** Needs only what is on `main`: Phase 1, 2D (`hierarchy`, `$mem`, generate names,
black boxes), 2E (`proc`, the `REFUSE` flag) and 2F (`opt`, `budget`). No stacked PRs. Two PRs:

| PR | Branch | Base | Tasks | Waits for |
|---|---|---|---|---|
| 3A-0 IR | `feat/3A-prov-hier` | `main` | 1 | Peter's review of the IR addition (spec §18, 1) |
| 3A-1 pass | `feat/3A-flatten` | `main` after 3A-0 | 2 → 3 → 4 → 5 | 3A-0; Task 5 needs Peter's answers to §18 questions 2 and 3 for its doc edits |

If Peter rejects the IR addition (spec §6, "Fallback"), Task 1 is dropped and Tasks 3 and 5 lose
their provenance-path steps; nothing else changes.

**Model/effort** (PHASE0 §9; PHASE3 row 3A: Opus `high`):

| Task | Implementer | Reviewer |
|---|---|---|
| 1 `odin3_prov_derive_hier` | Opus `high` | Opus `high` (Peter reviews: IR change) |
| 2 Pre-flight graph, caps, registration | Opus `high` | Opus `high` |
| 3 Expansion: joins, names, attributes, provenance | Opus `high` | Opus `high` |
| 4 `flatten-diff`, `flatten-sim`, golden fixtures | Opus `high` (fixtures: Sonnet `high`) | Opus `high` |
| 5 Fuzz, VTR sweep, calibration, doc amendments | Opus `high` (sweep runs: Sonnet `high`) | Opus `high` |
| Whole-branch review before each PR | — | read-only critique agent (CLAUDE.md rule 12) |

Effort cues: `/model opus` + `/effort high` for Tasks 1–4; `/model sonnet` + `/effort high` for fixture
bulk and the Task 5 runs; `/clear` between tasks.

**Lanes.** One lane (this one); the five tasks are serial because Tasks 2–3 share `register.c`/
`expand.c`, and 4–5 need the pass. Heavy runs (Task 5) go through `tools/heavy-run` when the other
lanes are idle (memory `heavy-lock-blocks-builds`); everything before it is unit work.

**Pre-flight conflict note.**
1. `src/passes/builtin.c`/`builtin.h`: one new table entry (`&odin3_pass_flatten`), defined in
   `src/passes/flatten/register.c`. The entry's line is independent of the other lanes' (3B adds `lower`,
   3C `hardmap`).
2. `docs/PASSES.md`: one row (Task 2); the golden-test column is completed in Task 5. `docs/PHASE3.md`
   edits are confined to the checklist line and a new decisions entry (3B/3C lanes append theirs;
   the later PR resolves).
3. `src/ir/prov.[ch]` and `tests/unit/test_prov.c`: Task 1 only.
4. `CMakeLists.txt`: append-only hunks (unit tests, `tests/tools` Python tests, the `cli_flatten` CLI test,
   the YOSYS055 CTests, the `sweep` CTests through `odin3_add_sweep_test`).
5. `tools/verilog-gen`: Task 5 adds `--flatten` beside the other lanes' generators (append-only).
6. `tests/unit/proc_build.[ch]` (hand-building IR from RTLIL) is used, not changed. Unit fixtures
   build hierarchy with `odin3_module_create` + `odin3_module_add_port` + `odin3_node_create_connected`
   in a small shared helper `tests/unit/flatten_fixture.[ch]` created in Task 2.

**Spec points made exact here** (agent defaults, reversible; Task 5 logs them in PHASE3):
- (a) **Entry point:** `odin3_status odin3_flatten_run(odin3_pass_ctx *ctx, odin3_design *design,
  const odin3_flatten_opts *opts)`, declared in `src/passes/flatten/flatten.h`. `opts` is `{ const
  char *separator; }` (NULL = `.`).
- (b) **Per-instance state** is one struct `odin3_flatten_inst` (module pointers, `map` vec of `uint32_t`
  nets indexed by child net ID, prefix string, path string ID, the provenance operation) allocated once per
  root and reset per instance: no per-instance `malloc` in the hot loop.
- (c) **Redirect table** for merged parent nets is a `u64map` (dead net ID → kept net ID); lookups follow
  chains with a cap of the table size (cycle-free by construction, asserted).
- (d) **Worklist** is a `util/vec` of node IDs with a head index (FIFO).
- (e) **Name building** appends into one reusable `strbuf`; the interned string is looked up with
  `odin3_strtab_intern` once per object.
- (f) **Estimate constants** `B_node`, `B_pin`, `B_net`, `B_wire`, `B_rec` start as 160/32/64/96/96 and are
  replaced by Task 5's measured values; the estimate rounds up.
- (g) **The test hook** `flatten_test.h` mirrors `opt_test.h`/`elab_test.h`: `odin3_flatten_test_set_limits`.

---

### Task 1: `odin3_prov_derive_hier` (FL-10; IR change, Peter reviews)
**Files:** Modify `src/ir/prov.h`, `src/ir/prov.c`, `tests/unit/test_prov.c`, `docs/IR.md` (IR-12 wording only; §11 line added by Task 5); `CMakeLists.txt` unchanged (test exists).

**Interfaces:**
- Consumes: `odin3_prov_derive`'s existing store logic (`prov.c` ~ line 590–660; the hash already combines `rec->hier`, line 106).
- Produces: `odin3_status odin3_prov_derive_hier(const odin3_pass_ctx *ctx, odin3_prov_list parents, uint32_t hier_str, odin3_prov_id *out);` — as `odin3_prov_derive`, with the record's `hier` = `hier_str`. `odin3_prov_derive(ctx, p, out)` becomes a call with `hier_str` 0. Errors: `INVALID_ARG` for a bad context, op 0, no parents, a non-record parent, or a `hier_str` that is not a strtab ID (as `odin3_prov_source` checks, `prov.c` ~ line 422). Doc comment per the header convention: purpose, ownership (the record is owned by the design), failure modes.

- [ ] **Step 1: Write the failing tests** in `test_prov.c`: `derive_hier_sets_hier` (record's `hier` equals the string); `derive_hier_identity` (same parents, same op, different `hier` → two records; same `hier` → one); `derive_hier_zero_is_derive` (`hier_str` 0 gives the ID `odin3_prov_derive` gives); `derive_hier_bad_str` (a string ID past the table → `INVALID_ARG`, no record added); `derive_hier_no_op`/`_no_parents`/`_bad_parent` (as `odin3_prov_derive`); `derive_hier_dag` (parent IDs smaller; check rule 7 passes); `derive_hier_oom` (allocation sweep: no crash, no leak, store unchanged).
- [ ] **Step 2: Run them to see them fail.** `ctest --preset debug -R test_prov` — `odin3_prov_derive_hier` is undefined.
- [ ] **Step 3: Implement.** Factor the body of `odin3_prov_derive` into a static function taking `hier`; both entry points call it. No change to the hash, the record layout, hash-consing, tombstones, the index or `compact`.
- [ ] **Step 4: Rule check.** `ctest --preset debug` fully green; `tools/check-symbols.sh` (the function is not in `odin3.h`: the public ABI export list is unchanged).
- [ ] **Step 5: IR.md.** IR-12's "`flatten` adds it through its `DERIVED` records" sentence gets "(`odin3_prov_derive_hier`, 3A)". No other IR.md text.
- [ ] **Step 6: Lint and commit.** `tools/lint.sh`; commit `feat(ir): odin3_prov_derive_hier (3A FL-10)`; push; open PR 3A-0 titled "IR: odin3_prov_derive_hier (3A, for Peter's review)" with the spec §6 paragraph in the body. Merge when `ci` is green and Peter approves.

---

### Task 2: The pre-flight graph, caps, options, registration (FL-2, FL-3 pre-flight, FL-15, FL-17, FL-19)
**Files:** Create `src/passes/flatten/flatten.h`, `graph.c`, `register.c`, `flatten_test.h`, `tests/unit/flatten_fixture.[ch]`, `tests/unit/test_flatten_graph.c`; modify `src/passes/builtin.c`, `src/passes/builtin.h`, `docs/PASSES.md`, `CMakeLists.txt`.

**Interfaces:**
- Consumes: `odin3_design_module_end`, `odin3_module_get`, `odin3_module_celltype`, `odin3_celltype_get` (granularity), `odin3_design_top`, `odin3_attr_get`, `odin3_budget_check`/`_message`, `odin3_passrun_set_info`, the pass table (`odin3_pass_def`), `util/vec`, `u64map`, `strbuf`.
- Produces (all in `flatten.h`):
  - `typedef struct odin3_flatten_opts { const char *separator; } odin3_flatten_opts;`
  - `typedef struct odin3_flatten_graph odin3_flatten_graph;` — opaque; `odin3_status odin3_flatten_graph_build(const odin3_design *design, odin3_module_id top, odin3_flatten_graph **out);` builds the cell-type → module map, the instance edges (live module-type nodes only), the Kahn order and the roots; `void odin3_flatten_graph_free(odin3_flatten_graph *g);`.
  - `odin3_status odin3_flatten_preflight(const odin3_pass_ctx *ctx, const odin3_design *design, const odin3_flatten_graph *g);` — cycle report (`flatten: recursive instantiation: a -> b -> a`), saturating flat-size computation, caps, budget; logs and returns `INVALID_ARG` (cycle, caps) or the budget's status; mutates nothing.
  - `bool odin3_flatten_is_kept(const odin3_module *parent, odin3_node_id inst, const odin3_module *child);` — the `keep_hierarchy` rule of §7 (instance attribute or module attribute; INT/BITS non-zero, STRING non-empty).
  - the caps `ODIN3_FLATTEN_MAX_NODES` 2^26, `_PINS` 2^28, `_NETS` 2^28, `_WIRES` 2^26, `_ROOTS` 2^16, and the hidden hook in `flatten_test.h`.
  - `register.c`: the pass `flatten` (`processes = ODIN3_PASS_PROCESSES_REFUSE`), option parsing (`--separator <str>`; checks: non-empty, no blank, no `"`/`;`/`#`), the top check (`flatten: the design has no top module (run hierarchy)`), the INFO lines of spec §11. In this task `odin3_flatten_run` does the pre-flight and the roots/instance counting only, then logs `flatten: N instances to expand` and returns `ODIN3_OK` without mutating (Task 3 adds the expansion). The test fixture helper builds hierarchy in a few lines.
- Registers the row in `docs/PASSES.md` (view: RTLIL or netlist view → same; processes `REFUSE`; source files; tests `test_flatten_graph`, later the others).

- [ ] **Step 1: Write the failing tests** (`test_flatten_graph.c`): `graph_map_types` (modules found by cell type; a `blackbox`/`hard`/`word` node is not an instance); `graph_kahn_order` (diamond: a→b, a→c, b→d, c→d); `graph_cycle_a_b_a` and `graph_self_cycle` (error text verbatim, nothing mutated); `graph_cycle_via_kept` (still an error, as Yosys); `graph_sizes` (flat node/pin/net/wire counts for a 3-level tree with fan-out 3: equals a by-hand count, constant nodes and port nodes excluded); `graph_sizes_saturate` (2^40 instances counted without wrap); `caps_trip` (tiny caps by hook → error text with the numbers, design untouched); `budget_trip` (`odin3_budget_test_set_limit(1)` → the budget message); `kept_rule_values` (INT 0/1, BITS 0/1, STRING ""/"x", missing; instance vs module; top ignored); `opts_parse` (`--separator "::"`, empty, blank, unknown option → `unknown_option`); `no_top`; `refuse_processes` (the manager refusal, via `odin3_pass_run`); `roots_include_kept` (kept module that contains a non-kept instance is a root; order is module-ID order); `unused_modules_listed` (names capped at 8). OOM sweep over `graph_build`.
- [ ] **Step 2: Run them to see them fail.** `ctest --preset debug -R test_flatten_graph` — fails to link.
- [ ] **Step 3: Implement** `graph.c` (explicit queue; no recursion; map built in module-ID order), `register.c`, the pass-table entry, the PASSES.md row. The estimate function uses placeholder constants (point (f)).
- [ ] **Step 4: Run the gate.** `tools/build-locked --preset debug -j6 && ctest --preset debug -j4` is green (do not build while a heavy run holds the lock; wait for it).
- [ ] **Step 5: Lint and commit.** `feat(flatten): pre-flight graph, caps, pass registration (3A FL-3, FL-15)`.

---

### Task 3: The expansion — joins, names, attributes, constants, MEMID, provenance (FL-3…FL-14)
**Files:** Create `src/passes/flatten/expand.c`, `names.c`, `prov.c`, `tests/unit/test_flatten_expand.c`, `tests/unit/test_flatten_names.c`, `tests/unit/test_flatten_prov.c`, `tests/flatten/` (first fixtures + `README.md`), `tests/cli/` entry `cli_flatten`; modify `register.c` (calls the expansion), `flatten.h`, `CMakeLists.txt`, `docs/PASSES.md`.

**Interfaces:**
- Consumes: Task 2; `odin3_node_create_connected`, `odin3_node_delete`, `odin3_net_create`, `odin3_net_merge`, `odin3_pin_*`, `odin3_wire_create` (explicit net vector), `odin3_attr_foreach`/`_set`/`_get`, `odin3_module_find_*` (name lookups for the `_N` rule), `odin3_prov_begin_op`, `odin3_prov_derive_hier` (Task 1), `odin3_prov_get` (the original record's `hier`), `odin3_strtab_intern`, `odin3_celltype_get`/the builtin IDs for the constant types, `odin3_budget_check` (every 2^16 created nodes).
- Produces:
  - `odin3_status odin3_flatten_expand_root(odin3_flatten_ctx *fc, odin3_module *root);` — the §3.3 per-instance loop with the FIFO worklist; `odin3_flatten_ctx` holds the design, pass context, options, graph, the redirect table, shared-constant nets, the `MEMID` set, the counters.
  - static helpers, each ≤ 60 lines: `record_instance`, `delete_instance`, `build_net_map` (§3.4 table), `join_port_bit`, `map_constants`, `copy_nodes`, `copy_wires`, `copy_attrs`, `rewrite_memid`, `check_joins` (§3.7).
  - `names.c`: `odin3_flatten_name(fc, kind, prefix, name, &out)` (prefix + separator + name, then the `_N` rule against the root's per-kind name maps and net aliases), `odin3_flatten_unnamed_label(inst_node_id)` (`$c<ID>`), `odin3_flatten_memid(fc, memid, &out)` (the `MEMID` set).
  - `prov.c`: `odin3_flatten_path(fc, inst_name, &path_str)` (parent path + `/` + name; root name for roots), `odin3_flatten_derive(fc, orig_prov, &out)` (the §6 `hier` = path + original `hier` minus its first component; parents `[orig, instance]`; zero rules).
  - pass-run information `roots=… instances=… kept=… nodes=… renamed=…` set once.
  - Debug post-conditions of spec §12 (asserted).

- [ ] **Step 1: Write the failing tests**, exactly the spec §14.1 list that belongs here: `flat_simple`, `flat_alias_order`, `flat_nested_names`, `flat_two_instances`, `flat_unconnected`, `flat_constants`, `flat_multidriver_error`, `flat_const_output_error`, `flat_bus`, `flat_blackbox_kept`, `flat_keep_hierarchy_inst`/`_module`/`_value_kinds`, `flat_mem_memid`, `flat_params_copied`, `flat_attrs`, `flat_names_unique`, `flat_unnamed_instance`, `flat_prov`, `flat_prov_nested`, `flat_check_post`, `flat_feedthrough` (a child `output y = a`: one net, aliases in `odin3_net_merge` order, no cell), `flat_net_names` (BLIF-style net names become bare aliases `u1.n~19`), `flat_generate_names` (`g[1].u2.x`), `flat_hand_body` (`tests/golden/blif/hand_body.blif` through `read_blif; flatten`: the `sub` instance disappears, `bb` stays a `blackbox` instance), `flat_hand_ports` (likewise), `flat_determinism` (two runs and reversed module order), `flat_oom` (injection at every allocation of `flat_simple`: no crash, no leak, `NO_MEMORY`, check rules 1–3, 5, 6, 8–11 hold). The fixture helper from Task 2 builds IR in a few lines each.
  - `cli_flatten`: `odin3 -p "read_blif tests/flatten/nested.blif; flatten; write_blif out.blif"` produces the committed `nested.flat.blif` (top model compared); `odin3 -p "read_verilog tests/flatten/hier.v --top top; proc; flatten; write_rtlil out.il"` produces the committed expected.
- [ ] **Step 2: Run them to see them fail.** `ctest --preset debug -R "test_flatten|cli_flatten"` — the pass does not expand yet.
- [ ] **Step 3: Implement** `expand.c`, `names.c`, `prov.c` in that order, running the tests after each: first without provenance and names (tests `flat_simple`, `flat_constants`), then names and aliases, then attributes and `MEMID`, then the errors, then provenance. Keep every function inside the gate (`odin3_flatten_inst` carries the state).
- [ ] **Step 4: Writers.** Run the fixtures through `write_rtlil`, `write_verilog` and `write_blif`: the aliased nets must print without dropping or duplicating (spec §16, item 2). A writer bug found here is fixed in its own PR (`fix/<writer>-alias`), and the fixture waits for it; record in the PR description.
- [ ] **Step 5: Run the gate** (`tools/build-locked --preset debug -j6 && ctest --preset debug -j4`) and `check` FULL before/after on every fixture (automatic in Debug).
- [ ] **Step 6: Lint and commit.** `feat(flatten): expansion, joins, names, provenance (3A FL-3…FL-14)`. Push. (The PR is opened after Task 5, so 3A-1 is one pass, one PR.)

---

### Task 4: `flatten-diff`, `flatten-sim` and the golden fixtures (FL-18; spec §14.2–§14.4)
**Files:** Create `tools/flatten-check/flatten-diff`, `flatten-sim`, `flatten_common.py`, `tests/tools/test_flatten_diff.py`, `tests/tools/test_flatten_sim.py`, `tests/flatten/*.v`, `tests/flatten/*.blif`, `tests/flatten/expected/*`, `tests/flatten/diff_expect.txt`; modify `CMakeLists.txt` (YOSYS055-gated and plain CTests), `docs/PASSES.md`.

**Interfaces:**
- Consumes: the `odin3` CLI (`read_verilog`, `read_blif`, `hierarchy`, `proc`, `opt`, `flatten`, `write_rtlil`, `write_blif`, `sim`); Yosys 0.55 (`ODIN3_YOSYS055`); 2F's `tools/opt-check` equivalence helper (`equiv_make … equiv_simple -undef`, imported, not changed); `tools/sim-check` conventions (seeded vectors, reset rules); `tools/netlist-compare`.
- Produces:
  - `flatten-diff [--top T] [--yosys PATH] [--odin3 PATH] [--keep DIR] FILE...` — §14.3: reads both RTLIL outputs, compares module `T` (public wire names and widths, public cell names/types, `$mem` MEMIDs, private cell type counts, remaining instances) and runs the equivalence script; prints a table with the result class per file; exit 0/1/2 as `netlist-compare`; unexpected differences must be listed in `tests/flatten/diff_expect.txt` (`file: reason`), an unlisted difference fails and a listed one that no longer differs also fails (stale entry).
  - `flatten-sim [--top T] [--vectors N] [--seed S] FILE...` — §14.4: `sim` before and after `flatten` on the same vectors; classes `equal`, `differs`, `skipped:<cell>` (never counted as pass), exit codes.
  - `tests/flatten/` fixtures (first batch): nesting 3 deep, two instances of one module, a parameterized child (`#(.W(4))` and `#(.W(8))` → two specializations), generate loop with instances and a generated name `g[1].u`, feed-through, constant-tied output, unconnected port, `inout` bus, `$mem` in a child (two instances), a `keep_hierarchy` module and a `keep_hierarchy` instance, a `(* blackbox *)` module, a `read_techlib lib/vtr.o3lib` hard `adder`/`single_port_ram` instance in a child, an unnamed-instance BLIF.
  - the CTests: `flatten_diff_micro` (all of `tests/micro/verilog` plus the fixtures; YOSYS055), `flatten_sim_micro` (no Yosys needed), `flatten_golden` (expected files), `tools_flatten_diff_unit`.
- [ ] **Step 1: Write the failing tests.** `test_flatten_diff.py`: comparison of two small hand-written RTLIL files (equal; a missing public wire; a different cell type; a stale `diff_expect` entry; a listed difference), exit codes, `--keep`; `test_flatten_sim.py`: a fake `odin3` (a script printing canned traces) to test the harness (`equal`, `differs`, `skipped`).
- [ ] **Step 2: Run them to see them fail.** `.venv/bin/python -m unittest tests/tools/test_flatten_diff.py`.
- [ ] **Step 3: Implement** the two tools (stdlib only; `ruff` and `mypy --strict` clean), then generate the expected files with Odin III and **review each against Yosys by hand** before committing (the tool is the check; expected files are not self-validating).
- [ ] **Step 4: Run the real legs** on the fixtures and `tests/micro/verilog`: `flatten-diff` and `flatten-sim`; fix `flatten` bugs found (in Task 3's files, with a new test each); list unavoidable differences in `diff_expect.txt` with a reason.
- [ ] **Step 5: Gate.** Build, `ctest --preset debug -j4`, `tools/lint.sh`.
- [ ] **Step 6: Commit.** `feat(flatten): flatten-diff, flatten-sim, golden fixtures (3A §14)`. Push.

---

### Task 5: Fuzz, the VTR sweep, calibration, doc amendments (FL-18; spec §14.5, §14.6, §17)
**Files:** Create `tools/flatten-check/fuzz_flatten`, `tests/flatten/vtr_expect.txt`; modify `tools/verilog-gen` (`--flatten`), `src/passes/flatten/graph.c` (estimate constants), `CMakeLists.txt` (`fuzz_flatten_quick`, `flatten_vtr_sweep` via `odin3_add_sweep_test`, label `sweep`), `docs/IR.md`, `docs/DESIGN.md`, `docs/PHASE3.md`, `docs/PASSES.md`; `.github/workflows/nightly.yml` — only a commented step.

**Interfaces:**
- Consumes: Tasks 2–4; `tools/heavy-run` and `odin3_add_sweep_test()` (2E-1); `tools/vtr-qor` is not used (no VPR here); the 19 designs under `$VTR_ROOT/vtr_flow/benchmarks/verilog`.
- Produces:
  - `verilog-gen --flatten SEED`: random hierarchies per spec §14.5 (depth ≤ 6, fan-out ≤ 4, parameterized children, generate instances, feed-through, constant-tied output, one unconnected port, `inout`, `keep_hierarchy`).
  - `fuzz_flatten --cases N --seed S`: each case through `flatten-diff`'s checks (names, cells), `flatten-sim`, and `check`; a failing case is written to `tests/fuzz/flatten/` as a reproducer (path printed) and, once fixed, kept as a fixture. `fuzz_flatten_quick` (2,000 cases, no Yosys needed for the sim leg; the Yosys leg when `ODIN3_YOSYS055`).
  - the sweep (§14.6): for the 19, smallest first, `read_verilog --vtr-compat --nolatches; proc; opt; flatten; check`, recorded as a table (wall, peak RSS, flat nodes/nets/wires, records, bytes per object, `kept=`, `renamed=`, node counts by cell type vs Yosys `flatten; stat`). The table is written to `docs/PHASE3.md` (decisions entry) verbatim.
  - **calibration:** `graph.c`'s estimate constants (`B_node`… `B_rec`) replaced by the measured values (rounded up, with the measurement's date and design in a comment), and the test `estimate_not_below_actual` (the estimate on a fixture is ≥ the real `odin3_design_bytes_reserved` delta).
  - **docs (spec §17):** `docs/IR.md` (IR-12 sentence; §11 line "PHASE3 #29 (3A spec)"; §12 "`flatten`" removed), `docs/DESIGN.md` (§6 `flatten` line; §13 #6 note), `docs/PHASE3.md` (checklist 3A ticked when the PR merges; the stale "hierarchical BLIF goldens" sentence replaced by the fixtures; a decisions entry #29 listing every agent default of the spec and Peter's answers to §18; the script string change if Peter approved question 3, with the same edit in the 3H spec/plan script strings), `docs/PASSES.md` (golden-test column).
- [ ] **Step 1: Write the failing tests.** `tests/tools/test_verilog_gen_flatten.py` (generator output is deterministic per seed and elaborates under `read_verilog; hierarchy; proc`); `test_fuzz_flatten.py` (harness with a planted bug: a fake `odin3` that drops a wire → the case is reported and written).
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement** the generator and the harness; run `fuzz_flatten_quick`; fix every bug found with a fixture and a unit test first.
- [ ] **Step 4: The sweep (controller, heavy).** `tools/heavy-run --label sweep ... flatten_vtr_sweep`, smallest first, when the other lanes are idle; the large ones (`LU32PEEng`, `mcml`, `bgm`) last. Record the table; apply the calibration; `kept=` must be 0 on all 19 (spec §7), else investigate.
- [ ] **Step 5: Doc amendments** as listed; `tools/lint.sh` (it checks the docs' rule scripts too).
- [ ] **Step 6: Whole-branch critique** by a read-only agent (CLAUDE.md rule 12); triage findings; list the rejected ones in the PR description.
- [ ] **Step 7: Commit, push, open PR 3A-1** "3A: flatten pass" (body: the spec/plan links, the evidence table, the divergence table link, the rejected review findings), self-merge when `ci` is green (decision #17).

---

## Review focus

1. Task 3's join table (§3.4): feed-through, constant output, two bits of one net. Fuzzed.
2. Aliases through the writers (Task 3 step 4).
3. Provenance cost (records per instance) and the budget estimate (Task 5 calibration).
4. The weak failure state (spec §12): confirm that no non-OOM, non-§3.7 failure can occur after the first mutation.
5. `flatten-diff`'s comparison is only as good as its "expected differences" list: every entry needs a reason.

## Verification summary (what "done" means)

- `ctest --preset debug` green, including `test_flatten_*`, `cli_flatten`, `flatten_diff_micro` (YOSYS055), `flatten_sim_micro`, `fuzz_flatten_quick`, `tools_flatten_*`.
- `flatten-diff`: `equivalent` or listed on every micro and fixture; `flatten-sim`: `equal` on every non-RAM design; the sweep table in PHASE3.
- `tools/lint.sh` clean; `check` FULL before and after in Debug on every run.
- Two runs byte-identical (`write_rtlil`, `write_blif`); reversed module order gives the same names and cells.
- PASSES.md row, IR.md/DESIGN.md/PHASE3.md amendments merged with the PR.
