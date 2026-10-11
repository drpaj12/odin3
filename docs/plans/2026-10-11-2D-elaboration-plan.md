# 2D Elaboration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the parsed store of one `read_verilog` run into word-level IR with processes. Stage 1 (Verilog only) builds the elaborated store: module lookup, top selection, parameters with Yosys's precedence, specializations, `generate`, constant functions, implicit nets, non-ANSI port merging and the REJECT/IGNORE diagnostics. Stage 2 (shared with the Phase 5 slang adapter) binds that store and emits wires, ports, word cells, instances, drivers, memories, `$proc` process cells, inlined functions and initial values. The IR changes A (nine registry cells), B (the `$proc` cell) and C (port range and sign) and `write_rtlil` land first. The evidence is per-row goldens in both latch modes, the Yosys 0.55 bridge, micro and VTR sweeps, fuzzers, the mcml.v/lenet.v benchmark and the 2H `--x-report` mode. No `proc`, no `opt`.

**Architecture:** `src/frontends/verilog/elab1*.c` is stage 1: a run context (`odin3_elab1`) with a definition table, a specialization table keyed by resolved parameter values, and a FIFO worklist. Each specialization is walked twice: pass E evaluates and records every generate decision in a log, and pass W replays the log and emits the elaborated nodes bottom-up through the 2A builders. `src/frontends/elab/` holds what both front ends share:
- the 4-state constant values (`cval*.c`), the two-sweep EL-13 typer (`ctype.c`), and the evaluator and statement interpreter behind a host callback interface (`ceval*.c`);
- the binder, which fills the 2A symbol table (`bind.c`);
- stage 2's module emitter (`elab2*.c`): ports for every specialization before any body, then per module wires → mem2reg decision → initial values → instances → assignments → processes → wired-net gates → `$mem`, with a driver table checked before anything is attached;
- the process generator (`pg*.c`). It follows Yosys's `ProcessGenerator`, builds each body off-IR with `lv`/`rv` maps and undo logs, and makes one `$proc` node per block with `odin3_node_create_connected`.

`src/ir` gains the nine word cells and the `$proc` type: real `I`/`O` ports, an opaque `ODIN3_VAL_PROC` body blob checked by `verify`, and a per-module counter. It also gains the port fields of change C. The pass manager refuses `processes = REFUSE` passes on a design with processes. `src/backends/rtlil` writes RTLIL, processes included, so Yosys 0.55 can lower 2D's output for comparison.

**Tech Stack:** C17, `src/util`, `src/ir` (+ IR changes A/B/C), `src/ast` (store, builders, number values, symbol table, check, dump, print), 2C (`odin3_read_verilog_files`, the `read_verilog` pass, `tests/unit/verilog_test.h`), 2B-1 project record (`top`, `top_at`, `params`), the 1D pass manager, CLI and ABI, 1E simlib word helpers (`sim/word.h`), Unity, Python 3 stdlib (bridge, sweeps, generators, `--x-report`), libFuzzer (clang, optional), Yosys 0.55 (`$VTR_ROOT/build/bin/yosys`; 0.69 only for the nightly drift job), ABC through `tools/equiv-check`, Icarus (simlib cases).

**Spec:** `docs/specs/2026-10-11-2D-elaboration-design.md` v3. It is binding: EL-n and § numbers below refer to it, IR changes A/B/C (§7.2, §8.1, §6.2) are part of it, and its §9 message texts are verbatim requirements. Also:
- `docs/PHASE2.md` #13: the rulings, decisions 1–13 and the x exit-bar ruling. #4: the exit bar. #6: D1–D5. #8: the 2A carry-overs, among them `odin3_symtab_remap` in `compact`.
- `docs/IR.md` (IR-1…IR-18, §9 rules) and the op registry `src/ir/cells/`.
- The 2A spec: §4.3 slot table, §4.4 flags, §4.5 numbers, §4.6 elaborated form, §7 attributes and §8 symbol table. Also `src/ast/*.h`.
- What 2C hands over: its spec §3.6, §4.2 and §7.3, its plan points (i), (l) and (m), and `tests/verilog/reject-2d/`/`ignore-2d/`.
- `docs/specs/2026-10-09-2C-verilog-coverage.md` (rows `L1`…`S4`: 100 MUST, 25 SHOULD, 10 IGNORE, 24 REJECT).
- 1E (`docs/specs/2026-10-09-1E-sim-design.md`: simlib semantics, the `simulate`/`sim_scratch_bytes` contract) and 1D (`docs/specs/2026-10-09-1D-abi-design.md`).
- The oracle is Yosys 0.55 as pinned by VTR (`frontends/ast/simplify.cc`, `genrtlil.cc`, `backends/rtlil/rtlil_backend.cc`).

**Ordering and bases.** `main` has 2A (#33), 2B-1 (#34), 2B-2 (#36) and the 2C spec and plan (#35, #37). Three branches are in flight:
- 2C-1 (`feat/2C-parser`): the lexer and grammar, Tasks 1–3 of 7 done.
- 2C-2 (`feat/2C-read`, not started): `read_verilog`, `--parse-only` and the stub `odin3_elab_run` in `src/frontends/verilog/elab.h` with `elab_stub.c`.
- 2B-3 (`feat/2B-passes`): the passes and CLI, Tasks 10–11 done.

Seven PRs, as spec §18:

| PR | Branch | Base | Tasks | Waits for |
|---|---|---|---|---|
| 2D-1 IR | `feat/2D-ir` | `origin/main` now | 1–6 | **Nothing.** Tasks 1–3 (the IR changes) touch only `src/ir`, `docs/IR.md`, `docs/DESIGN.md` §5.2/§6 and their tests. The only exceptions are Task 2's mechanical initializer fixes in test files and Task 3's two public enum values in `odin3.h` plus one-line `ODIN3_VAL_PROC` switch arms. None of these files is touched by 2C-1, 2C-2 or 2B-3, so they start **before 2C lands**. Tasks 4–6 touch the pass manager, `builtin.c`, the writers, `src/api` and `odin3.h`, which 2B-3 and 2C-2 also edit (conflict notes 4–8). Rebase onto whichever of them has merged before Task 4 starts; otherwise whichever lands second rebases. |
| 2D-2 constants | `feat/2D-ceval` | `origin/main` now | 7–9 | Nothing for Tasks 7–8 (hand-built trees). Task 9's interpreter tests parse sources with 2C-1's `vt_parse`, so rebase onto `main` after 2C-1 merges, before Task 9. |
| 2D-3 hierarchy | `feat/2D-hier` | `main` after 2C-2 and 2D-2 | 10–13 | 2C-2 merged (`odin3_read_verilog_files`, the pass, `elab.h`/`elab_stub.c`, `odin3_ast_library_units_from`). Stage 1 never touches the IR, so 2D-1 is **not** required, and neither is 2B-3. |
| 2D-4 stage-1 content | `feat/2D-content` | `main` after 2D-3 | 14–16 | 2D-3 |
| 2D-5 stage-2 module | `feat/2D-stage2` | `main` after 2D-1 and 2D-4 | 17–20 | 2D-1 (cells, ports, `write_rtlil`) and 2D-4. 2B-3 only for the `--elab-only` + `--project` sugar (conflict note 9). |
| 2D-6 processes | `feat/2D-proc` | `main` after 2D-5 | 21–24 | 2D-5 |
| 2D-7 evidence | `feat/2D-evidence` | `origin/main` now; rebased after each 2D PR merges | 25–29 | It grows from 2D-3 and opens last, after 2D-6. Task 26's corpus run needs 2B-3's `project-fixtures compare`. Task 28's fuzz target needs the `ODIN3_FUZZ` block that 2B-3 or 2C-2 creates. |

"Grows from 2D-3" works like this. The row sources and hand-written stage-1 goldens (Task 25, no code) are written on `feat/2D-evidence` while 2D-3 is in review. Before each of 2D-3…2D-6 opens, the controller runs the evidence branch's runners against that PR's head and quotes the result in its description. The evidence CTests themselves land with 2D-7. Task 25's Python bridge `elab-equiv` is written first on the evidence branch and cherry-picked into 2D-6 for Tasks 21, 22 and 24's bridge CTests, as 2B did with its Python harness.

**Model/effort** (PHASE0 §9; PHASE2 row 2D is Opus `xhigh`, review by Peter on semantics; `high` once settled, as spec §18 says):

| Task | Implementer | Reviewer |
|---|---|---|
| 1 IR change A: nine word cells | Opus `high` (registry entries) | Opus `xhigh` |
| 2 IR change C: port range and sign | Opus `xhigh` | **Fable**, else Opus `xhigh` |
| 3 IR change B: the `$proc` cell | Opus `xhigh` | **Fable**, else Opus `xhigh` |
| 4 Passes and processes, writer refusals, `compact` symtab remap | Opus `high` | Opus `xhigh` |
| 5 `write_rtlil` | Opus `high` | Opus `high` |
| 6 ABI v5 | Opus `high` | Opus `high` |
| 7 Values, operators, the Yosys 0.55 constant table | Opus `xhigh` | Opus `xhigh` |
| 8 Expression typing | Opus `xhigh` | **Fable**, else Opus `xhigh` |
| 9 Evaluator, interpreter, system functions | Opus `xhigh` | Opus `xhigh` |
| 10 Stage-1 context, entry, lookup, top | Opus `xhigh` | Opus `xhigh` |
| 11 Parameters, specializations, passes E/W, push-down | Opus `xhigh` | **Fable**, else Opus `xhigh` |
| 12 Instances, connections, arrays, black boxes | Opus `xhigh` | Opus `xhigh` |
| 13 Binder | Opus `high` | Opus `xhigh` |
| 14 Generate and scope names | Opus `high` | Opus `xhigh` |
| 15 Declarations, ports, implicit nets, normalisations | Opus `high` | Opus `high` |
| 16 REJECT/IGNORE diagnostics, cap, handover on | Opus `high` | Opus `high` |
| 17 Stage-2 context, wires, ports, provenance, budget, `--elab-only`, `odin3_ast_elaborate` | Opus `xhigh` | Opus `xhigh` |
| 18 Expressions and selects to word cells | Opus `xhigh` | **Fable**, else Opus `xhigh` |
| 19 Instances, gates, assignments, drivers, wired nets, tri-state | Opus `xhigh` | Opus `xhigh` |
| 20 Memories (decision, `$mem`, reads) | Opus `xhigh` | Opus `xhigh` |
| 21 Process generator core | Opus `xhigh` | **Fable**, else Opus `xhigh` |
| 22 `if`/`case`, case temporaries, `full_case`/`parallel_case`, lvalue selects | Opus `xhigh` | Opus `xhigh` |
| 23 Loops, functions and tasks, continuous-context calls | Opus `xhigh` | Opus `xhigh` |
| 24 Memory writes, initial values, `$readmem`, T9/T10 | Opus `xhigh` | Opus `xhigh` |
| 25 `elab-check`, rows, goldens, the bridge | Opus `high` (bulk generated goldens: Sonnet `high`) | Opus `high` (re-derives every hand golden) |
| 26 Project corpus, handover close-out, micro sweeps | Opus `high` | Opus `high` |
| 27 VTR sweeps, `bench_elab`, PHASE2 entry | Opus `high` (bench runs: Sonnet `high`) | Opus `high` |
| 28 Fuzzer and differentials | Opus `high` | Opus `high` |
| 29 `equiv-check --x-report` and the Parmys preview | Opus `high` | Opus `high` |
| Whole-branch review before **each** PR (2D-1 … 2D-7) | — | **Fable**, else Opus `xhigh` |

Effort cues:
- Say `/model opus` + `/effort xhigh` at the start of 2D-1 Task 2, and keep `xhigh` through 2D-3 and through 2D-5/2D-6.
- Say `/effort high` for Tasks 1, 4–6, 13–16 and 25–29.
- Say `/model sonnet` + `/effort high` for bulk golden generation and the bench runs.
- Say `/clear` between tasks (one task per session).

**Parallel lanes** (disjoint files; land one at a time on each branch):
- **2D-1:** {1 ‖ 2} → 3 → {4 ‖ 5} → 6. Tasks 1 and 3 both append to `builtin.c`. Task 2 changes the port-def macros Task 3's cell uses, so it lands first.
- **2D-2:** 7 → 8 → 9, in parallel with all of 2D-1.
- **2D-3:** 10 → 11 → 12, with 13 alongside 11–12. The binder works on hand-built elaborated stores and shares no file with stage 1.
- **2D-4:** 14 → 15 → 16. They share pass E/W.
- **2D-5:** 17 → 18 → 19 → 20.
- **2D-6:** 21 → 22 → 23 → 24. They share `odin3_pg`.
- **2D-7:** 25 starts with 2D-3 (sources and hand goldens). 29 (Python only) can run any time. 26 → 27 after 2D-6, then 28.

Stage 1 (2D-3/2D-4) and the IR (2D-1) are independent lanes until 2D-5.

**Pre-flight conflict note.**
1. `CMakeLists.txt`: every task appends sources to `odin3_core`, `odin3_add_unit_test(...)` lines and CTests. These are append-only hunks; keep both sides. Task 25 adds the `ODIN3_YOSYS055` discovery block once (point (p)). Every Yosys-0.55 CTest sits inside it.
2. `src/ir/cells/builtin.c` and `cells.h`: Task 1 appends the nine cells and Task 3 appends `$proc` (point (f)). Append-only.
3. `src/ir/celltype.h` and `src/ir/module.h`: Task 2 appends `is_signed` to `odin3_port_def` and four fields to `odin3_port_spec`. Task 3 touches only the flag comment. Every positional initializer of either struct (cells macros, `tests/unit/*.c`) gains the new trailing values in Task 2. `-Wmissing-field-initializers` under `-Werror` finds them all.
4. `include/odin3/odin3.h`:
   - Task 3 adds `ODIN3_VAL_PROC` (4) and `ODIN3_CT_PROCESS` (32).
   - Task 4 adds the public `ODIN3_PASS_PROCESSES_*` constants and renames `odin3_plugin_pass.reserved[0]` to `processes`.
   - Task 6 bumps the ABI to v5 and adds `odin3_module_get_proc_count`.
   - Task 10 adds `odin3_ast_get_library_units_from`.
   - Task 17 adds `odin3_elab_options` and `odin3_ast_elaborate`.
   - 2B-3 adds `odin3_design_has_project`, `odin3_pass_set_revision` and `odin3_pass_set_diag_paths` in other hunks.
   All hunks are additive. The v5 header-comment line is written once (Task 6) and extended by Tasks 10 and 17 (point (c)).
5. `src/passes/manager.{h,c}`: Task 4 appends `uint8_t processes` to `odin3_pass_def` and the refusal in `run_def`. 2B-3 extends `odin3_pass_options` and `run_def` (diag paths). Different hunks, but both in `run_def`: the refusal goes **before** `odin3_pass_run_begin`, and 2B-3's diag-path line stays first.
6. `src/passes/builtin.c`, `builtin.h`, `write_formats.c`, `sim_pass.c`: Task 4 adds a `processes` value to every positional `odin3_pass_def` initializer and the remap in `compact`. Task 5 adds `write_rtlil` (in `write_formats.c` and the `odin3_builtin_passes[]` table). 2B-3 adds `read_project`, `dump_project` and `preprocess` (`project_pass.c`). 2C-2 adds `read_verilog`. Whichever lands second gives the other's new defs their `processes` value (point (x)).
7. `src/api/{pass.c,node.c,module.c,ast_abi.c}`, new `src/api/elab_abi.c`: Tasks 4, 6, 10 and 17. 2B-3 edits `src/api/pass.c` and `design.c` (setters): different functions. `plugins/python/odin3.py` reads the cdef verbatim; Task 6 only adds the kind name.
8. `src/backends/{blif,json,verilog,dot}/writer.c` and `src/backends/json/emit.c`: Task 3 adds the `ODIN3_VAL_PROC` switch arms. Task 4 adds the refusal at each writer's entry. Nothing else in flight touches them.
9. `src/cli/main.c` and `tests/cli/test_cli.sh`: Task 17 adds `--elab-only` and `cli_elab_only*`. 2C-2 adds `--parse-only`; 2B-3 adds `--project`/`-f`/`--revision`/`--diag-paths`. `--elab-only` mirrors 2C's `--parse-only` code path. Whichever of 2D-5 and 2B-3 lands second adds the `--project`/`-f` + `--elab-only` append rule, as 2C conflict note (5) does for `--parse-only`.
10. 2C-2's files: `src/frontends/verilog/elab.h`, `elab_stub.c` and `read_verilog.c`.
    - Task 10 changes the `odin3_elab_run` signature (point (a)), deletes `elab_stub.c`, adds the three flags to `read_verilog`'s option parse and usage, and changes the call.
    - Task 16 changes `tools/parse-check/run-reject` and `tests/tools/test_run_reject.py` (per-case pending, point (l)). It may correct a `reject-2d/*.err` or `ignore-2d/*.warn` text only to match a §9 table row, quoting that row in the commit message.
11. `src/ast/*`: 2D adds no AST API. It consumes 2C-2's `odin3_ast_library_units_from`. `docs/specs/2026-10-09-2A-ast-design.md` gets §19's two amendments: Task 15 the §4.6 pointer to EL-4, Task 22 the §7 D1 sentence. 2C-2 makes its §9 (b) there. These are different paragraphs.
12. Docs:
    - `docs/IR.md` (Tasks 2, 3, 17) and `docs/DESIGN.md` (Tasks 1, 3, 5, 10). PHASE0 `docs/` rules: only these tasks edit them.
    - `docs/PASSES.md`: Tasks 4, 5, 10 and 17; 2B-3 and 2C-2 add rows. Rows are independent lines; keep both.
    - `docs/specs/2026-10-09-2C-verilog-coverage.md` E12 (Task 8).
    - `docs/specs/2026-10-09-1D-abi-design.md` (Task 4).
    - `docs/PHASE2.md` is append-only: Task 27 adds 2D's entry as the next free number (#14 unless 2C took it).
13. `tests/unit/simlib_word_cases.h` and `tools/sim-check/gen_simlib_cases.py`: Task 1 only. Regenerate, never hand-edit.
14. 2C-2's `tools/verilog-gen/*` and the fuzz CMake block: Task 28 extends them (`--elab`, `fuzz_elab`). `tools/project-fixtures/project_fixtures.py` (2B-3): Task 26 adds the `--with-frontend` bridge mode. `tools/equiv-check/equiv_check.py`: Task 29 only.
15. Golden directories `tests/elab/rows/`: Task 25 writes them. A later task that finds a hand golden wrong fixes it only with the EL rule or Yosys 0.55 run that proves it quoted in the commit message, and the reviewer re-derives that file.

**Spec points made exact here** (agent defaults, reversible; Task 27 logs them in PHASE2):

- (a) **Options reach the entry.** 2C's stub signature has no options channel. The entry becomes `odin3_status odin3_elab_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_ast *parsed, const odin3_elab_opts *opts)`.
  - `read_verilog` parses `--nolatches`, `--nomem2reg` and `--auto-blackbox` into `opts` and leaves `top`/`top_at` 0.
  - Step (1) fills them from `--top` (`odin3_pass_get_options().top`, interned, `top_at` 0), else from the project record's `top`/`top_at`.
- (b) **ABI entry.** `odin3_pass_ctx` is not in the ABI, so the ABI form is `odin3_status odin3_ast_elaborate(odin3_design *design, odin3_ast *ast, const odin3_elab_options *opts)`.
  - The store must be held by `design` (made with `odin3_design_new_ast`), sealed and of form `ELABORATED`; anything else is `INVALID_ARG`.
  - The provenance run is the store's (`odin3_ast_get_run`), from which the call builds the internal context (`op` 0).
  - It runs EL-2 steps 4–5. Step 6 is moot because the design already holds the store. `opts->top` and `auto_blackbox` are stage-1 options, ignored here.
- (c) **One ABI bump**, v4 → v5, in Task 6 (2D-1). The first incompatible changes are there: two enum values and a reserved slot that gains a meaning. 2D-3 and 2D-5 only append declarations under v5 and extend its comment line.
- (d) **IR change C has no ABI mirror.** No ABI function creates modules or ports, and `odin3_plugin_port` stays frozen (§6.2).
- (e) **`ODIN3_VAL_PROC` payload** is one blob in the value's `bits`/`len` (`len` = bytes).
  - Layout: a header of six `uint32_t` counts (pbits, compares, actions, cases, switches, syncs), then the six arrays in that order as the §8.1 C structs, each array at an 8-byte-aligned offset.
  - `odin3_proc_body_pack` builds it zero-filled (arena memory is zeroed and the arena is `max_align_t`-aligned), so padding bytes are 0.
  - Hence `odin3_value_copy`, `odin3_value_equal` (byte compare), `compact` and tombstones need nothing beyond `has_payload` accepting `PROC`.
- (f) **Table order.** The nine cells (`$neg $xnor $reduce_xnor $reduce_bool $logic_not $logic_and $logic_or $shiftx $pow`), then `$proc`, are appended after `$sop` (index 52). Built-in IDs 0–52 do not move; the count becomes 63.
- (g) **`$proc` type.** Ports are `I` and `O` (`ODIN3_PORT_VEC` on `I_WIDTH`/`O_WIDTH`). Parameter defaults are `I_WIDTH` 0, `O_WIDTH` 1 and an empty `BODY`, which `verify` rejects, so a default node cannot exist. `ODIN3_CT_PROCESS` = 32. A plugin or local type carrying the flag is `INVALID_ARG` ("only the built-in $proc may have ODIN3_CT_PROCESS").
- (h) **A sync on an `OUT` position is legal** for edge and level syncs. §8.1 is normative and carries re-review N1. §14.1's "a sync signal that is an OUT position" among the rejected bodies is a stale v2 phrase. Task 3 asserts acceptance and rejects only an `ALWAYS` sync with a signal, or a signal that is not one `IN`/`OUT` position.
- (i) **Refusal point.** The `processes` check runs in `run_def` before `odin3_pass_run_begin`, so a refused pass opens no run.
  - The message names the first module, in creation order, whose count is nonzero.
  - `odin3_pass_def.processes` is the last member (`uint8_t`).
  - The plugin slot is `const uint8_t *processes`, read once at registration; NULL means `REFUSE` and a value above 1 is `INVALID_ARG`.
- (j) **Headers.** CLAUDE.md asks for one header per module, while §12 names only `elab.h`, `elab_internal.h` and `elab_test.h`. The split is:
  - `src/frontends/elab/cval.h`: values and operators.
  - `ceval.h`: typing, evaluator and interpreter.
  - `elab.h`: options, caps and stage APIs.
  - `elab_internal.h`: binding, the stage-2 context, emission, drivers and memories.
  - `pg.h`: the process generator.
  - `elab_test.h`: limits hook.
  - `src/frontends/verilog/elab1.h`: the stage-1 context.
  - `src/frontends/verilog/elab1_inst.c`: instances and connections, added to §12's list.
- (k) **The run-level iteration cap crosses the seam without a field.** Stage 2 starts its `ODIN3_ELAB_MAX_TOTAL_ITERATIONS` count at the number of indexed `GEN_BLOCK`s in the elaborated store, one per generate-loop iteration stage 1 kept. The error count needs no carry, since stage 2 never starts after a stage-1 error.
- (l) **Not-yet-built stages.**
  - Until Task 17, `odin3_elab_stage2` is a stub (`src/frontends/elab/elab2_stub.c`). It logs 2C's exact text `elaboration is not available yet (2D)` and returns `PARSE`.
  - From Task 17 until Tasks 21/23, reaching an `ALWAYS`, an `INITIAL`, a continuous-context call or a mem2reg variable-index read outside a process logs the same text and skips the construct.
  - Task 16 makes `run-reject --elaborate` judge per case: expected line found → pass; else the stub text present → **pending** (listed, not failed); else → fail. Exit 77 only when every case is pending.
  - Task 24 asserts zero pending.
- (m) **`--x-report`** is 2H's (§14.6 item 4, §18). It is built here (Task 29) because 2H grows alongside 2D and the brief asks for it. It is used only by a report-only preview of the Parmys comparison and never gates a 2D PR. The defined-output miter (§14.6 item 5) stays in 2H's backlog. §14.6 item 2 (Yosys's x rules) is 2F's, and item 3 (the BLIF writer's `unconn` convention) is 2H's; 2D's part is item 1 (x kept explicit), asserted by Task 18 (`$shiftx`, the wildcard `===`) and Tasks 21–22 (x constant pins).
- (n) **Memory caps for sweeps** (WSL rule).
  - Every child a sweep driver starts (odin3, Yosys, equiv-check, `bench_elab`) runs under `systemd-run --user --scope -q -p MemoryMax=<cap> -p MemorySwapMax=0`. Where that is unavailable, Release binaries (never ASan) run under `prlimit --as=<cap>`.
  - `--jobs J` is refused when `J × cap` exceeds 60% of `MemTotal`.
  - Defaults: J = 1, cap 4 GiB; `lenet.v` and its Yosys run get 8 GiB alone.
  - A child killed by its cap is reported `out of memory (cap N)`, never retried in parallel.
- (o) **Micro `--top`** (EL-7, ruling 6) is the first `.model` of `~/odin3-ws/golden/k6_frac_N10_frac_chain_mem32K_40nm/regression/verilog/<group>/<leaf>/<leaf>.parmys.blif`, read by `tools/elab-check/micro-top`.
- (p) **Yosys 0.55 discovery.** `find_program(ODIN3_YOSYS055 yosys HINTS $ENV{VTR_ROOT}/build/bin $ENV{HOME}/odin3-ws/external/vtr-verilog-to-routing/build/bin NO_DEFAULT_PATH)`, and `yosys -V` must contain `Yosys 0.55`. Without it no 0.55 CTest is registered: CI has none, so they run locally and nightly. 0.69 is the existing `ODIN3_YOSYS`, used only by the nightly drift job. Every generated table and golden records `yosys -V` in its header.
- (q) **Name collisions** get `$2`, `$3`, … in specialization creation order (FIFO), so names are deterministic. A name longer than 128 bytes is hashed **before** the collision check.
- (r) **RTLIL process names.** A `$proc` node prints as `$proc$<file>:<line>$<ID>`, with file and line from the node's SOURCE record (the file's given path), else `$proc$<ID>`.
- (s) **One body printer.** `odin3_proc_print` prints a body as an RTLIL `process` block through a net-spelling callback. `write_rtlil` passes its spelling; `odin3_node_get_param_text(BODY)` uses the same printer with `write_rtlil`'s spelling.
- (t) **`bench_elab`** is a new `tests/bench/bench_elab.c` (§15's name), not a mode of `bench_ast`.
- (u) **Stage-1 tests** are split into `test_elab_stage1_lookup.c`, `_params.c`, `_inst.c`, `_gen.c`, `_decl.c` and `_diag.c`. Stage-2 tests are `test_elab_stage2_wires.c`, `_expr.c`, `_drive.c`, `_mem.c`, `_proc.c`, `_case.c`, `_call.c` and `_init.c`.
- (v) **Warn once.** A construct elaborated several times warns once (§9.2). The key is `(parsed node ID << 32 | message-format index)` in a `u64map` on the stage-1 context. Stage 2 keys on the elaborated node's `loc`.
- (w) **Info texts §9 leaves open:**
  - `body of black box 'm' is ignored`
  - `module 'foo' is not defined; it becomes an implicit black box (--auto-blackbox)`
  - `implicit black box 'foo': port 'a' is 4 bits at its first instance; this connection is 8 bits` (an error)
  - `value of parameter 'P' of cell type 't' is not an integer` (an error, EL-8 cell-type targets)
  - `--elab-only cannot be combined with a script; use read_verilog in the script` (CLI)
- (x) **`processes` values for other branches' passes.** `read_project`, `dump_project`, `preprocess` and `read_verilog` are `OK` (they never read the IR as logic). Every other existing pass is as §8.1 lists.
- (y) **`$readmem` paths.** A relative path resolves against the directory of the resolved path of the `FILE` buffer holding the call's `loc`. An absolute path is used as given (DESIGN §4.0).
- (z) **Text and count fixes.**
  - §4.7's "$clog2 needs a constant argument" and §9.1's `<what>` form disagree. Use §9.1's form: `$clog2 argument is not constant (it reads 'x')`.
  - §14.2's hand-written list names **54** rows, not "≈ 40". All 54 are hand-written.
  - §14.1's `test_ir_module.c` does not exist yet: Task 2 creates it.
  - The constant table reads values back from `write_rtlil` `connect` lines of module outputs, because `localparam`s do not appear in RTLIL. Each case is `assign y<k> = <expr>;` in one module per batch.

## Global Constraints

- **Earlier constraints.**
  - C17 with the `-Werror` flag set.
  - Functions ≤ 60 lines, CCN ≤ 15, ≤ 5 parameters, no adjacent convertible parameters (use request structs).
  - **No recursion in `src/`**: the typer, evaluator, interpreter, generate expansion, hierarchy walk, binder, emitter and process generator all use explicit stacks or worklists. Tests may recurse in reference models.
  - No VLA; no `goto` but `cleanup:`; `abort` only in IR check.
  - No `NOLINT`; no `.clang-tidy` or lizard change; never `--no-verify`.
  - `odin3_` snake_case, with `elab1_*`/`elab2_*`/`pg_*`/`cval_*` for file-static helpers.
  - Named constants; only `src/util` containers; deterministic output; single-threaded.
- **IR discipline (EL-2, EL-28).**
  - Stage 1 never touches the IR.
  - Stage 2 checks every user-input rule before it attaches the object concerned, so an IR call never returns `INVALID_ARG` on user input: one that does is a 2D bug.
  - Ports of every specialization exist before any module body.
  - Nets are merged only in emission steps 3–5.
  - Each `$proc` node is created whole by one `odin3_node_create_connected`.
  - A failed stage 2 leaves valid, incomplete modules. `odin3_check_design` FULL passes after every failing test.
- **Oracle.** Every "verified" assertion, constant table, golden and bridge run uses Yosys 0.55 (point (p)). 0.69 appears only in the nightly drift job, and the two are never mixed in one comparison (ORACLES.md).
- **Diagnostics.** Every located error and warning uses the exact §9 text, through `odin3_diag`: `file:line:col: error|warning|info: …`, lower case, names in `'…'`, no final period. DESIGN §4.0 kinds are written `<kind>: <detail>`. Every located-diagnostic test asserts the whole line through a capturing log sink. Errors are capped at 20 per run (`too many errors; elaboration stopped after 20`).
- **Caps, exact (§11)**, each a named constant in `src/frontends/elab/elab.h`, checked before the allocation it guards, reported as a located `PARSE` naming the cap, and lowered in tests through `odin3_elab_test_set_limits` (`elab_test.h`):

  | Cap | Value |
  |---|---|
  | `ODIN3_ELAB_MAX_ERRORS` | 20 |
  | `ODIN3_ELAB_MAX_WIDTH` | 2^20 |
  | `ODIN3_ELAB_MAX_CELL_PINS` | 2^22 |
  | `ODIN3_ELAB_MAX_LOOP_ITERATIONS` | 2^20 |
  | `ODIN3_ELAB_MAX_TOTAL_ITERATIONS` | 2^24 |
  | `ODIN3_ELAB_MAX_EVAL_STEPS` | 2^26 |
  | `ODIN3_ELAB_MAX_CALL_DEPTH` | 1024 |
  | `ODIN3_ELAB_MAX_HIER_DEPTH` | 1024 |
  | `ODIN3_ELAB_MAX_SPECIALIZATIONS` | 2^16 |
  | `ODIN3_ELAB_MAX_INSTANCES` | 2^24 |
  | `ODIN3_ELAB_MAX_ELAB_NODES` | 2^25 |
  | `ODIN3_ELAB_MAX_NET_BITS` | 2^26 |
  | `ODIN3_ELAB_MAX_MEMORY_BITS` | 2^30 |
  | `ODIN3_ELAB_MAX_INIT_BITS` | 2^26 |
  | `ODIN3_ELAB_MAX_READMEM_BYTES` | 2^28 |
  | `ODIN3_ELAB_MAX_MODULE_NAME` | 128 |
  | `ODIN3_ELAB_MAX_BYTES` | 60% of physical memory |

  Width, loop, node, net, memory-bit, depth and byte-budget caps are fatal.
- **Checks.**
  - Every elaborated store passes `odin3_ast_check(FULL)`, run by `odin3_elab_run` in Debug and under `--check`.
  - Every IR passes `odin3_check_design` FULL; the pass manager runs it after `read_verilog`.
  - Every test that builds IR asserts it.
  - Two runs of the same input give byte-identical `write_rtlil` output.
- **Tests first.**
  - Unity under ASan/UBSan.
  - An OOM sweep (`odin3_util_set_alloc_fail_after`) over each allocating entry point a task adds. It asserts `NO_MEMORY`, no leak, an unchanged store or valid IR.
  - Gates: `cmake --build --preset debug -j6 && ctest --preset debug -j4` green, `/lint` before each commit, commit (and the controller pushes) at each green checkpoint.
  - A read-only critique per task; a **Fable** whole-branch review before each PR.
- **Every public function** in `odin3.h` gets the purpose/ownership/failure paragraph (CLAUDE.md).
- **Python** (`tools/`, `tests/tools/`): stdlib only, `ruff check`, `mypy --strict`, `unittest`.
- **Sweeps** run smallest input first, sequentially by default under point (n)'s caps. No sweep runs while a build runs.

## Review Focus

1. **Parameterized instances as users write them.** Users expect what Yosys 0.55 gives:
   - `#(.W(8))` equal to the default shares the base module, named `m`, not a `$paramod`;
   - `defparam u1.W = 7` loses to `#(.W(5))`, with the warning;
   - of two `defparam`s, the first wins;
   - a project `param top.u1.W 3` beats all three.

   Pinned in Task 11 (`test_elab_stage1_params`: the five-level matrix, names `m`, `m$W=5`, `m$W=8`, `m$W=6`, `m$W=3`).
2. **Mixed-width arithmetic.** With 8-bit `a`, `b`, `y`, a user expects `y = (a + b) >> 1` to drop the carry: `$add` `Y_WIDTH` 8, as Yosys and IEEE. `y = (0 + a + b) >> 1` keeps it: 32-bit adds. An unsized `parameter P = 'hFFFF_FFFF` in a 64-bit context is all ones. Pinned in Task 8 (the typer's widths) and Task 18 (the cells' `Y_WIDTH`s).
3. **Clocked blocks that mix `=` and `<=`.** In `t = a & b; r <= t;`, `r` must take the new `t`, and `t` is a second output whose register `opt` later removes. `q <= q + 1` reads the held value. Pinned in Task 21 (the exact `$proc` bodies of T7 and T8 and the `OUT`-read semantics).
4. **Memories with non-zero bases and init files.** For `reg [7:0] m [1:4]`, `[4:1]` and `[-2:1]`, a write then a read at the same index must hit the same word. `$readmemh("init.hex", m)` reads from the source file's directory with `@` addresses. Pinned in Task 20 (`OFFSET` 1/1/−2, raw `RD_ADDR`) and Task 24 (write ports, `INIT` words, the bridge on all three ranges).
5. **Generate names users probe.** Users expect `genblk1.genblk1.x` for an `if … else if` chain, `genblk3[0].w` after it, `g[-1].t`, and a user wire `genblk1` that does not rename a later block. Pinned in Task 14 (stage-1 names) and Task 17 (IR wire names `g[0].t`, `genblk3[0].w`).

---

## PR 2D-1 — IR changes, pass flag, `write_rtlil`, ABI v5

### Task 1: IR change A — nine word cells (EL-31, §7.2)
**Files:**
- Modify `src/ir/cells/arith.c` (`$neg` unary with a hook; `$pow` binary, `simulate` NULL).
- Modify `src/ir/cells/logic.c` (`$xnor`, `$logic_and` and `$logic_or` binary, `$logic_not` unary, all with hooks).
- Modify `src/ir/cells/reduce.c` (`$reduce_xnor`, `$reduce_bool`, with hooks).
- Modify `src/ir/cells/shift.c` (`$shiftx` binary, `simulate` NULL).
- Modify `src/ir/cells/cells.h` (nine `extern`s) and `builtin.c` (append in point (f)'s order).
- Modify `tests/unit/test_ir_cells.c` and `test_sim_word.c`.
- Modify `tools/sim-check/gen_simlib_cases.py`: `BINARY` gains `$xnor $logic_and $logic_or`, `UNARY` gains `$neg $reduce_xnor $reduce_bool $logic_not`. Regenerate `tests/unit/simlib_word_cases.h`.
- Modify `docs/DESIGN.md` §5.2: the word list gains the nine cells.

**Consumes:**
- `ODIN3_BINARY`, `ODIN3_UNARY`, `ODIN3_BINARY_SIM`, `ODIN3_UNARY_SIM`.
- `odin3_cells_word_sim`, `odin3_cells_flag_store`, `odin3_cells_y_scratch`, `odin3_cells_word_load`.
- `odin3_word_neg`, `odin3_word_xor`, `odin3_word_not`, `odin3_word_is_zero` (`sim/word.h`).

**Produces:**
- `extern const odin3_celltype_def odin3_cell_neg, odin3_cell_xnor, odin3_cell_reduce_xnor, odin3_cell_reduce_bool, odin3_cell_logic_not, odin3_cell_logic_and, odin3_cell_logic_or, odin3_cell_shiftx, odin3_cell_pow;`
- Shapes are the shared tables:
  - unary (`$neg $reduce_xnor $reduce_bool $logic_not`): ports `A Y`, parameters `A_SIGNED A_WIDTH Y_WIDTH`;
  - binary (the other five): ports `A B Y`, parameters `A_SIGNED B_SIGNED A_WIDTH B_WIDTH Y_WIDTH`.
- Semantics are simlib's, under the existing 1E rules:
  - `$neg`: `Y = −A` at `Y_WIDTH` (A extended by `A_SIGNED`).
  - `$xnor`: `~(A ^ B)` at `Y_WIDTH`, operands extended as a binary cell.
  - `$reduce_xnor` (`~^A`), `$reduce_bool` (`|A`), `$logic_not` (`!A`), `$logic_and` (`(|A) && (|B)`), `$logic_or`: one bit, zero-extended to `Y_WIDTH`.
  - `$shiftx` and `$pow` are registered with `simulate = NULL` (§7.2).

**Tests:**
- `test_ir_cells`:
  - The expected table grows by nine. `odin3_builtin_celltypes[52] == &odin3_cell_sop`, `[53] == &odin3_cell_neg`, `[61] == &odin3_cell_pow`, and `odin3_builtin_celltype_count` is 62 (Task 3 makes it 63).
  - Each name is found by `odin3_celltype_find` in a fresh design.
  - A `$pow` node with `A_WIDTH` 4, `B_WIDTH` 3, `Y_WIDTH` 5 has 12 pins. `Y_WIDTH` −1 is `INVALID_ARG`.
  - A module holding a `$shiftx` node fails `odin3_sim_build` with "cannot simulate `$shiftx` (node …)".
- `test_sim_word` (hand cases plus the regenerated simlib cross-check over widths 1, 7, 64, 65 and the four sign pairs):
  - `$neg`: `A`=`4'b0011` unsigned, `Y_WIDTH` 4 → `1101`; `A_SIGNED` 1, `A`=`2'b10`, `Y_WIDTH` 4 → `0010`; the same unsigned → `1110`.
  - `$xnor`: `1100`,`1010` → `1001`; unsigned `A`=`2'b11` against `B`=`4'b0011` → `1111`.
  - `$reduce_xnor`: `3'b110` with `Y_WIDTH` 3 → `001`; `3'b100` → `000`.
  - `$reduce_bool`: `4'b0000` → `0`; `4'b0100` with `Y_WIDTH` 2 → `01`.
  - `$logic_not`: `0000` → `1`; `1000` with `Y_WIDTH` 2 → `00`.
  - `$logic_and`: `2'b10`,`3'b000` → `0`; `2'b10`,`3'b100` → `1`.
  - `$logic_or`: `00`,`000` → `0`; `00`,`001` → `1`.
- Every `simlib_word_cases.h` row equal. The header's generator line is unchanged except the lists.

### Task 2: IR change C — port range and signedness (EL-24, §6.2)
**Files:**
- Modify `src/ir/module.h`: `odin3_port_spec` appends `bool has_range; int32_t msb, lsb; bool is_signed;` after `prov`, and the contract paragraph is updated.
- Modify `src/ir/module.c` (`odin3_module_add_port`).
- Modify `src/ir/celltype.h`: `odin3_port_def` appends `bool is_signed;`.
- Modify `src/ir/cells/cells.h`: `ODIN3_PORT_BIT`/`_VEC`/`_FN` gain a trailing `false`.
- Every positional `odin3_port_def`/`odin3_port_spec` initializer the compiler reports gains its trailing values: `tests/unit/test_{ir_netlist,ir_celltype,ir_wire,ir_check,ir_compact,sim_word,sim_build,sim_cycle,blif_writer,json_writer,verilog_writer,dot_writer}.c`.
- Create `tests/unit/test_ir_module.c`. Modify `CMakeLists.txt`.
- Modify `docs/IR.md` IR-7: the port record carries range and sign, `has_range`, the derived width, and the two refusals.

**Consumes:** `odin3_wire_create` (already takes `msb`/`lsb`/`is_signed`) and the module's in-place `type_def`.

**Produces:** `odin3_module_add_port(odin3_module *module, const odin3_port_spec *spec, odin3_node_id *port_node)` with the §6.2 contract:
- Without `has_range`: behaviour as today (`[width−1:0]`), with `is_signed` applied.
- With `has_range`: width = `|msb − lsb| + 1`. A nonzero `width` that differs is `INVALID_ARG`, and so is `scalar` with a range wider than one bit.
- The port's wire gets `[msb:lsb]` and `is_signed`. The module cell type's `odin3_port_def` gets the width, `scalar` and `is_signed`.
- Nothing changes on failure.

**Tests (`test_ir_module.c`):**
- `input signed [0:7] a` (`width` 0, `has_range`, `msb` 0, `lsb` 7, `is_signed`) → OK:
  - the wire has `msb` 0, `lsb` 7, `signed`, width 8, and `odin3_wire_index(…, 0)` == 7;
  - the port node's `WIDTH` is 8;
  - `odin3_celltype_get(design, odin3_module_celltype(m))->ports[0]` has width 8, `is_signed`, `!scalar`.
- `[-1:2]` → width 4, `odin3_wire_index(…, 0)` == 2, `(…, 3)` == −1.
- Refusals:
  - `has_range` `[7:0]` with `width` 4 → `INVALID_ARG`, with port count, wire count and node count unchanged;
  - with `width` 8 → OK;
  - `scalar` with `[3:0]` → `INVALID_ARG`; `scalar` with `[5:5]` → OK, width 1.
- No range, `width` 3, `is_signed` → `[2:0]`, signed.
- All existing `test_ir_*` and writer tests pass unchanged apart from initializers.
- OOM sweep over `add_port` with a range: `NO_MEMORY`, module unchanged.

### Task 3: IR change B — the `$proc` cell (EL-33, §8.1)
**Files:**
- Create `src/ir/proc.h` and `src/ir/proc.c` (body types, pack/view, verify core, counter queries, printer).
- Create `src/ir/cells/proc.c` (the cell type) and `tests/unit/test_ir_proc.c`.
- Modify `src/ir/value.c`: `has_payload` and `odin3_value_valid` accept `ODIN3_VAL_PROC`; `odin3_value_equal` handles it as bytes.
- Modify `include/odin3/odin3.h`: `ODIN3_VAL_PROC` appended to `odin3_value_kind`, `ODIN3_CT_PROCESS = 32` appended to the `ODIN3_CT_*` enum, each with its comment.
- Modify `src/ir/celltype.c`: `ODIN3_CT_PROCESS` is accepted only on the built-in `$proc` (point (g)).
- Modify `src/ir/ir_internal.h` (module field `uint32_t proc_count`).
- Modify `src/ir/node.c`: the count changes on create, create_connected, delete and replace of a node whose type has the flag.
- Modify `src/ir/compact.c`: it keeps the count; tombstones copy `BODY` like any parameter.
- Modify `src/ir/cells/cells.h` and `builtin.c` (append `&odin3_cell_proc`).
- The one-line `ODIN3_VAL_PROC` arms `-Wswitch` requires, returning `INVALID_ARG` until Tasks 4/6: `src/ir/celltype.c`, `src/api/celltype.c`, `src/api/node.c`, `src/backends/json/emit.c`, `src/backends/verilog/writer.c`, `src/frontends/blif/reader.c`.
- Modify `CMakeLists.txt`.
- Modify `docs/IR.md`: a new **IR-19** (the cell, ports, parameters, body, the `OUT`-read rule, verify, merge/delete/compact behaviour, counter); IR-9 (the RTLIL view allows `$proc`, the netlist view does not); IR-10 (the `proc` value kind); IR-12 (`hier` is module-relative).
- Modify `docs/DESIGN.md`: §5.2 (`$proc` as the process cell) and §6 step 2 ("processes" = `$proc` nodes; `proc` before `opt`).

**Consumes:** Task 2 (port macros), `odin3_node_create_connected`, `odin3_net_const_value`, `odin3_strbuf`.

**Produces (`proc.h`):**
- §8.1's types verbatim: `odin3_pbit` with `ODIN3_PBIT_IN`/`_OUT`/`_ANY`/`_INDEX_MASK`, `odin3_pspan`, `odin3_sync_type`, `odin3_proc_action`, `odin3_proc_case`, `odin3_proc_switch`, `odin3_proc_sync`, `odin3_proc_body`.
- `enum { ODIN3_SWITCH_FULL_CASE = 1, ODIN3_SWITCH_PARALLEL_CASE = 2 };`, `enum { ODIN3_PROC_P_I_WIDTH, ODIN3_PROC_P_O_WIDTH, ODIN3_PROC_P_BODY };`, `enum { ODIN3_PROC_PORT_I, ODIN3_PROC_PORT_O };`.
- `odin3_status odin3_proc_body_pack(odin3_arena *arena, const odin3_proc_body *body, odin3_value *out)` builds point (e)'s blob.
- `bool odin3_proc_body_view(const odin3_value *val, odin3_proc_body *out)` gives views into the blob. It returns false for another kind, a short or inconsistent blob, or a count whose array overruns `len`.
- `typedef struct odin3_proc_why { char text[ODIN3_WIDTH_WHY_MAX]; } odin3_proc_why;`
- `odin3_status odin3_proc_body_verify(const odin3_proc_body *body, uint32_t i_width, uint32_t o_width, odin3_proc_why *why)` checks every §8.1 rule plus point (h). It is O(body), with one used-count array per record kind.
- `uint32_t odin3_module_proc_count(const odin3_module *module);`
- `bool odin3_design_has_processes(const odin3_design *design);`
- `odin3_module_id odin3_design_first_proc_module(const odin3_design *design);` (0 when none).
- `typedef odin3_status (*odin3_proc_sig_fn)(void *user, const odin3_module *module, const odin3_net_id *nets, uint32_t n, odin3_strbuf *out);` spells one span (LSB first; a none entry is `ANY`) as an RTLIL sigspec.
- `typedef struct odin3_proc_print_spec { odin3_proc_sig_fn sig; void *user; uint32_t indent; } odin3_proc_print_spec;` With `sig` NULL, constant nets print as merged runs (`2'1-`) and other nets as `\name` or `$n<ID>`, one entry each, in `{ … }` when there are several.
- `odin3_status odin3_proc_print(const odin3_module *module, odin3_node_id node, const odin3_proc_print_spec *spec, odin3_strbuf *out);` prints the RTLIL `process` block per §13.
- `odin3_cell_proc`:
  - `{"$proc", ODIN3_GRAN_WORD, ODIN3_CT_PROCESS, {I, O}, {I_WIDTH INT 0, O_WIDTH INT 1, BODY PROC empty}, proc_verify, NULL, NULL, NULL}`.
  - `proc_verify(const odin3_value *params)` checks `O_WIDTH` ≥ 1 and `I_WIDTH` ≥ 0, views the body and calls `odin3_proc_body_verify`. On failure it logs `$proc: BODY: <why>` and returns `INVALID_ARG`.

**Tests (`test_ir_proc.c`):**

Fixture P1 is `always @(posedge clk) q <= d;`, built by hand:
- nets named `clk`, `d`, `q`, `t0`;
- pins `I` = [clk, d], `O` = [q, t0];
- pbits `[OUT|1, OUT|0, OUT|1, IN|1, OUT|0, OUT|1]`;
- actions a0 (`t0 ← q`), a1 (`t0 ← d`), a2 (`q ← t0`);
- root case with a0, a1;
- sync `POSEDGE IN|0` with a2.

Cases:
- **Create and query.** `odin3_node_create_connected` → OK. `odin3_net_driver(q)` is `O` pin 0, the sinks of `d` include `I` pin 1, `odin3_module_proc_count` is 1, `odin3_design_has_processes` is true, and `odin3_check_design` FULL is OK.
- **Malformed bodies.** Each is `INVALID_ARG` with nothing created, and logs exactly `$proc: BODY: ` followed by:
  - `IN position 2 is out of range (I_WIDTH 2)`;
  - `action 1: lhs holds a non-OUT position`;
  - `action 0: OUT position 0 twice in one lhs`;
  - `action 0: lhs and rhs widths differ (1, 2)`;
  - `ANY outside a compare`;
  - `OUT position 2 is never assigned` (O_WIDTH 3);
  - `root case has compares`;
  - `switch 0: case 1 compare 0 has width 2; the signal has 1`;
  - `action 0 is used 2 times`;
  - `sync 0: an ALWAYS sync has a signal`;
  - `sync 0: the signal must be one IN or OUT position` (a signal of `ANY`);
  - `not a process body` (a `BITS` value; a blob 4 bytes short).
- **Point (h).** A `POSEDGE` sync on `OUT|0` (`always @(posedge a) a <= ~a`) is accepted.
- **Merge leaves the body alone.** `odin3_net_merge({keep d2, drop d})` moves `I` pin 1 to `d2`, and the `BODY` value stays byte-identical (memcmp before/after).
- **Rule 4.** A second `$add` driving `q` makes `check` report rule 4 (two drivers).
- **Compact.** A module with live P1 and a deleted copy is compacted: the live body is unchanged, and the tombstone has type `$proc` with a `BODY` equal (`odin3_value_equal`) to the deleted node's.
- **Counter.** Create 2 → 2; delete 1 → 1; `odin3_node_replace` → 1; compact → 1; delete → 0 and `has_processes` false.
- **Printer.** `odin3_proc_print` of P1 (node ID 1, default spec, indent 0) is exactly:

  ```
  process $proc$1
    assign \t0 \q
    assign \t0 \d
    sync posedge \clk
      update \q \t0
  end
  ```

  P2 adds `switch \s` with flags FULL_CASE, a case with compare `IN|k` on the shared `$_CONST1_` net and `t0 ← d`, and a default case. It prints `attribute \full_case 1`, `switch \s`, `case 1'1`, `assign \t0 \d`, `case`, `end` in that order.
- A plugin cell type with `ODIN3_CT_PROCESS` → `INVALID_ARG` with point (g)'s text.
- OOM sweep over `pack` and `create_connected`.

### Task 4: Passes and processes, writer refusals, `compact` symtab remap (EL-33 pass flag; EL-2 remap)
**Files:**
- Modify `src/passes/manager.h`: `uint8_t processes` is appended to `odin3_pass_def`, and the paragraph says what it means.
- Modify `src/passes/manager.c`: the refusal in `run_def`, before `odin3_pass_run_begin` (point (i)).
- Modify `src/passes/builtin.c` and `write_formats.c` so every `odin3_pass_def` initializer gets its value:
  - `OK`: `read_blif`, `read_techlib`, `check`, `stats`, `compact`, `hierarchy`, `write_verilog_ast`, plus point (x)'s passes when present;
  - `REFUSE`: `write_blif`, `write_json`, `write_verilog`, `write_dot`, `sim` (`sim_pass.c`).
- `compact` calls `odin3_symtab_remap` after each module (below).
- Modify `src/backends/{blif,json,verilog,dot}/writer.c`: each `odin3_*_write` refuses a design with processes before opening the path.
- Modify `include/odin3/odin3.h`: public `enum { ODIN3_PASS_PROCESSES_REFUSE = 0, ODIN3_PASS_PROCESSES_OK = 1 };`, and `odin3_plugin_pass` gets `const uint8_t *processes; void *reserved[3];` (layout unchanged), with the paragraph updated.
- Modify `src/api/pass.c` (read the slot).
- Modify `tests/unit/test_pass_manager.c`, `test_pass_outputs.c`, `test_abi_ext.c`.
- Modify `docs/PASSES.md`: a "Processes" paragraph under the table listing the `OK` passes. The `compact` row loses "Not yet wired" and says the remap runs.
- Modify `docs/specs/2026-10-09-1D-abi-design.md` (`odin3_pass_def.processes`, plugin slot) and `docs/DESIGN.md` §6 step 2 (the order, now enforced).

**Consumes:** Task 3 (`odin3_design_has_processes`, `odin3_design_first_proc_module`); 2A `odin3_ast_symtab`, `odin3_symtab_remap`, `odin3_design_ast_count/_ast_at`; `odin3_module_compact`'s map.

**Produces:**
- **Refusal.** A `REFUSE` pass on a design with processes logs `pass '<name>': module '<m>' has processes; run proc first` and returns `ODIN3_ERR_INVALID_ARG` without running and without a pass run.
- **Writers.** Each logs `module '<m>' has processes; run proc first` and returns `INVALID_ARG`; the path is untouched.
- **`compact`.** After each module's `odin3_module_compact(module, &map)`, it calls `odin3_symtab_remap(st, module_id, &map)` for every held store whose `odin3_ast_symtab` is non-NULL, then `odin3_compact_map_free`.

**Tests:**
- Fixture: a design with module `m` holding Task 3's P1, and a test pass `t_refuse` whose body increments a counter.
- `odin3_pass_run(design, "t_refuse", …)` → `INVALID_ARG`, the log line exactly `pass 't_refuse': module 'm' has processes; run proc first`, counter 0, pass-run count unchanged.
- An `OK` test pass runs. `stats` and `check` run.
- `write_blif x.blif` through the manager → the refusal line naming `write_blif`.
- `odin3_blif_write`, `odin3_json_write`, `odin3_verilog_write` and `odin3_dot_write` called directly → `INVALID_ARG`, `module 'm' has processes; run proc first`, and no file at the path.
- `sim 4 1` → refused.
- Plugin pass `processes` NULL → `REFUSE`; `&one` → `OK`; `&two` (2) → registration `INVALID_ARG`.
- Remap: an elaborated store handed to the design with a symtab symbol whose `ir_wire` is wire 5 of `m`. Delete wire 3 and run `compact`: the symbol's `ir_wire` is 4.
- Every existing pass-manager test still passes.

### Task 5: `write_rtlil` (EL-48, §13)
**Files:**
- Create `src/backends/rtlil/writer.h`, `writer.c` and `tests/unit/test_rtlil_writer.c`.
- Modify `src/passes/write_formats.c` (`odin3_pass_write_rtlil`, `processes` OK), `builtin.h` (extern) and the built-in table.
- Modify `CMakeLists.txt`: unit test, plus CTest `rtlil_yosys_roundtrip` inside the 0.55 block.
- Modify `docs/PASSES.md` (row `write_rtlil <file>`, 2D, RTLIL view → —) and `docs/DESIGN.md` §9 (row "RTLIL (Yosys text) | — | yes | word level with processes; the Yosys bridge for 2D–2F tests").

**Consumes:** Tasks 2–3; `odin3_attr_foreach`, `odin3_net_primary`, `odin3_net_alias_next`, `odin3_net_const_value`, `odin3_prov_sources`, `odin3_pass_write_text` (2B-3's atomic writer if merged, else the writers' existing atomic helper).

**Produces:**
- `odin3_status odin3_rtlil_text(const odin3_design *design, odin3_strbuf *out);`
- `odin3_status odin3_rtlil_write(const odin3_design *design, const char *path);` (atomic)
- `odin3_status odin3_rtlil_sig(void *user, const odin3_module *module, const odin3_net_id *nets, uint32_t n, odin3_strbuf *out);` (the `odin3_proc_sig_fn` that point (s) shares)
- `extern const odin3_pass_def odin3_pass_write_rtlil;`

Format (§13; field order as Yosys 0.55's `rtlil_backend.cc`):
- One `module \name` per module in creation order, with its attributes first.
- Live wires in ID order: `wire [width W] [upto] [offset O] [input|output|inout K] [signed] \name`, K = the 1-based port index.
- `wire $n<ID>` for each net that is no wire's bit.
- Sigspecs:
  - MSB first;
  - consecutive bits of one wire's primary merge into `\w [hi:lo]`, and a whole wire is `\w`;
  - constant-net runs are `<n>'<bits>`, `ANY` is `-`;
  - several chunks go in `{ … }`.
- Cells:
  - `cell <type> <name>`, where an unnamed node is `$c<ID>`, a type's name starting with neither `$` nor `\` gets `\`, and instance nodes use their module's name;
  - `parameter \P <value>` in definition order (INT decimal, BITS `<n>'<bits>`, STRING `"…"` with escapes; `MEMID` gets a `\` prefix);
  - `connect \PORT <sig>` in port order;
  - `end`.
- Port and constant nodes are not written.
- `$proc` nodes print through `odin3_proc_print` with `odin3_rtlil_sig`, named per point (r), after the cells.
- Then `connect <alias> <primary>`, one per alias run, in alias order.

**Tests (`test_rtlil_writer.c`):**
- Fixture, created in this order: module `m` with ports `input [3:0] a` (port 1) and `output signed [0:1] y` (port 2); wire `b` `[3:0]` merged onto `a`; an unnamed `$add` (node 3: A = `a`, B = const `4'b0011`, `Y_WIDTH` 2 on `y`); then the constant nodes. The text contains exactly these lines in this order:
  - `module \m`
  - `  wire width 4 input 1 \a`
  - `  wire width 2 upto output 2 signed \y`
  - `  wire width 4 \b`
  - `  cell $add $c3`
  - `    parameter \A_SIGNED 0` (then `\B_SIGNED 0`, `\A_WIDTH 4`, `\B_WIDTH 4`, `\Y_WIDTH 2`)
  - `    connect \A \a`
  - `    connect \B 4'0011`
  - `    connect \Y \y`
  - `  end`
  - `  connect \b \a`
  - `end`
- A wire `[8:1]` prints `wire width 8 offset 1 \w`.
- A one-bit select of `a[2]` prints `\a [2]`; a concatenation prints `{ \a [3:2] 2'x1 }`.
- P1 inside a module prints `  process $proc$1` at indent 2.
- A `$mem` node prints `parameter \MEMID "\\m"`, `parameter \OFFSET 1` and `parameter \INIT 16'xxxxxxxxxxxxxxxx` (an empty INIT prints as `0'`).
- Two writes give identical bytes.
- CTest `rtlil_yosys_roundtrip`:
  - the unit test writes its fixtures to `build/<preset>/rtlil/*.il`;
  - Yosys 0.55 runs `read_rtlil F; write_rtlil G` and `read_rtlil F; proc; opt; write_blif H`, each exiting 0, smallest first.

### Task 6: ABI v5 (EL-47)
**Files:**
- Modify `include/odin3/odin3.h`: `ODIN3_ABI_VERSION = 5`; the comment line `ABI v5 (Phase 2, 2D): $proc processes and process counts, value kind PROC, pass processes flag; later in v5: library units, odin3_ast_elaborate`; `odin3_module_get_proc_count`.
- Modify `src/api/module.c`, and `src/api/node.c` (param text and kind of `BODY`, through `odin3_proc_print` with `odin3_rtlil_sig`).
- Modify `plugins/python/odin3.py` (value-kind name `PROC`).
- Modify `tests/unit/test_abi_core.c`, `test_abi_ir.c`, `test_abi_values.c` and `tests/tools/test_python_walk.py`.

**Consumes:** Tasks 3–5.

**Produces:**
- `odin3_status odin3_module_get_proc_count(const odin3_design *design, uint32_t module, uint32_t *count);` with the paragraph: O(1), `INVALID_ARG` as the conventions say.
- `odin3_node_get_param_kind` of `BODY` gives `ODIN3_VAL_PROC`.
- `odin3_node_get_param_text` of `BODY` gives the process text (interned, as the 1D text convention).

**Tests:**
- `odin3_abi_version()` == 5.
- The example plugin built for v4 → `ODIN3_ERR_ABI_MISMATCH`.
- `get_proc_count` of P1's module → 1; module 99 → `INVALID_ARG`.
- Param kind of index 2 → 4.
- Param text equals Task 3's P1 text with `\` names as `write_rtlil` spells them.
- The Python walk prints the `$proc` node with kind `PROC` and its body text.

## PR 2D-2 — constants: values, typing, evaluator, interpreter

### Task 7: Values, operators and the Yosys 0.55 constant table (EL-12, EL-14, EL-15)
**Files:**
- Create `src/frontends/elab/elab.h`: caps, `odin3_elab_opts`, and the stage API declarations added later (Tasks 10, 17).
- Create `src/frontends/elab/elab_test.h`: `odin3_elab_limits` and `odin3_elab_test_set_limits`.
- Create `src/frontends/elab/cval.h`, `cval.c` (construction, conversion, resize, select, concat, print, hash) and `cval_arith.c` (operators).
- Create `tools/elab-check/gen-const-table` (Python) and `tests/elab/const_table.txt`, generated, with a header of `# gen-const-table seed 2026 count N` and `# yosys: <yosys -V>`.
- Create `tests/unit/test_elab_cval.c` and `tests/tools/test_gen_const_table.py`.
- Modify `CMakeLists.txt` (sources; `test_elab_cval` with `ODIN3_ELAB_DATA="${PROJECT_SOURCE_DIR}/tests/elab"`; the Python test in `python_tools`).

**Consumes:** `odin3_arena`, `odin3_ast_literal` (`odin3_ast_number_value`), `odin3_ast_number`, `odin3_value`, `odin3_hash_bytes`, `odin3_strbuf`.

**Produces (`cval.h`):**
- §4.1's `odin3_cval` and `enum { ODIN3_CVAL_SIGNED = 1, ODIN3_CVAL_UNSIZED = 2, ODIN3_CVAL_STRING = 4 };`.
- Construction and conversion:
  - `odin3_status odin3_cval_new(odin3_arena *arena, uint32_t width, uint8_t flags, odin3_bit fill, odin3_cval *out);` (`INVALID_ARG` for width 0 or above the width limit: a caller bug, since the evaluator checks first)
  - `odin3_status odin3_cval_from_literal(odin3_arena *arena, const odin3_ast_literal *lit, odin3_cval *out);`
  - `odin3_status odin3_cval_from_string(odin3_arena *arena, odin3_bytes text, odin3_cval *out);` (L10: 8 bits per byte, first character most significant, `""` = `8'h00`, `STRING` set)
  - `odin3_status odin3_cval_from_i64(odin3_arena *arena, int64_t value, uint32_t width, bool is_signed, odin3_cval *out);`
  - `odin3_status odin3_cval_copy(odin3_arena *arena, const odin3_cval *src, odin3_cval *out);`
  - `odin3_status odin3_cval_to_value(odin3_arena *arena, const odin3_cval *val, odin3_value *out);` (BITS, one byte per bit)
  - `odin3_status odin3_cval_to_number(odin3_arena *arena, const odin3_cval *val, odin3_ast_number *out);` (payload `width` 0 when `UNSIZED`, §2.4)
- Resizing and inspection:
  - `odin3_status odin3_cval_resize(odin3_arena *arena, const odin3_cval *val, uint32_t width, bool is_signed, odin3_cval *out);` follows EL-13. An `UNSIZED` value repeats its top bit; otherwise sign extension when `is_signed`, else zero. Truncation keeps the low bits. The result is sized, with `SIGNED` = `is_signed`.
  - `odin3_bit odin3_cval_bit(const odin3_cval *val, uint32_t pos);`
  - `odin3_bit odin3_cval_truth(const odin3_cval *val);` (0, 1 or X; z reads as x)
  - `bool odin3_cval_is_known(const odin3_cval *val);`
  - `bool odin3_cval_to_i64(const odin3_cval *val, int64_t *out);` (false with x/z or out of range; signed by `SIGNED`)
  - `bool odin3_cval_equal(const odin3_cval *lhs, const odin3_cval *rhs);` (width, flags, both planes)
  - `uint64_t odin3_cval_hash(const odin3_cval *val, uint64_t seed);`
  - `odin3_status odin3_cval_print(const odin3_cval *val, odin3_strbuf *out);` gives EL-9's spellings: decimal for a 32-bit signed known value, else `<w>'<s>b<bits>`; a `STRING` value as `"…"` with `"` and `\` escaped.
- Operators:
  - `typedef enum odin3_cop { ODIN3_COP_ADD, _SUB, _MUL, _DIV, _MOD, _POW, _AND, _OR, _XOR, _XNOR, _SHL, _SHR, _SSHL, _SSHR, _EQ, _NE, _EQX, _NEX, _LT, _LE, _GT, _GE, _LAND, _LOR, _NEG, _POS, _NOT, _LNOT, _RAND, _RNAND, _ROR, _RNOR, _RXOR, _RXNOR } odin3_cop;`
  - `typedef struct odin3_cop_args { odin3_cop op; const odin3_cval *a, *b; uint32_t width; bool is_signed; } odin3_cop_args;`
  - `odin3_status odin3_cval_op(odin3_arena *arena, const odin3_cop_args *args, odin3_cval *out);` computes `RTLIL::const_*` semantics at `width`. Arithmetic, bitwise and unary operations extend both operands to `width` by `is_signed` (an unsized operand repeats its top bit). Comparisons extend both to `width` and give 1 bit. Logical operations and reductions read operands as they are and give 1 bit. Shifts extend `a` only; `b` is an unsigned amount, except `POW`, which reads `b`'s own `SIGNED`.
- Parts and mux:
  - `typedef struct odin3_cval_part { const odin3_cval *val; int64_t lo; uint32_t width; } odin3_cval_part;`
  - `odin3_status odin3_cval_select(odin3_arena *arena, const odin3_cval_part *part, odin3_cval *out);` (positions outside the value read x)
  - `odin3_status odin3_cval_concat(odin3_arena *arena, const odin3_cval *parts, uint32_t n, odin3_cval *out);` (MSB part first)
  - `odin3_status odin3_cval_replicate(odin3_arena *arena, const odin3_cval *val, uint32_t count, odin3_cval *out);`
  - `typedef struct odin3_cval_mux_args { const odin3_cval *cond, *a, *b; } odin3_cval_mux_args;`
  - `odin3_status odin3_cval_mux(odin3_arena *arena, const odin3_cval_mux_args *args, odin3_cval *out);`: an x `cond` merges bit by bit, equal bits kept, others x.
- Word-parallel planes for widths ≤ 64; schoolbook word loops above.

**Produces (`elab.h`):**
- §11's caps as named constants.
- §2.2's `odin3_elab_opts`.
- `typedef struct odin3_elab_limits { uint32_t max_errors, max_width, max_cell_pins, max_loop_iterations, max_total_iterations, max_eval_steps, max_call_depth, max_hier_depth, max_specializations, max_instances, max_elab_nodes, max_net_bits, max_init_bits, max_readmem_bytes; uint64_t max_memory_bits, max_bytes; } odin3_elab_limits;`
- `void odin3_elab_test_set_limits(const odin3_elab_limits *limits);` (process-wide for the test run; NULL restores defaults; each value clamped to its default)

**Produces (`gen-const-table --yosys Y --seed S --count N --out F`):**
- For random cases it writes one module per batch of 256 with outputs `y<k>`, and `assign y<k> = <expr>;` with operands written as sized literals at the case's width and sign.
- It runs `Y -q -p "read_verilog -sv b.v; write_rtlil b.il"` and reads `connect \y<k> <const>`.
- Each case line is `<op> <width> <s> <a> <b> <result>`, in Verilog binary with x/z (`-` for a missing `b`).
- Mix: every op in `odin3_cop`, widths {1, 7, 64, 65, 200}, both signs, ⅓ of operands with x/z bits; deterministic per seed.
- It refuses a Yosys whose `-V` lacks `0.55`.

**Tests:**
- `test_elab_cval`:
  - Every line of `const_table.txt` (≥ 3,000) is equal through `odin3_cval_op`.
  - Division: `8'd7 / 8'd0` → `8'bxxxxxxxx`; `%` by zero → all x; signed `-7 / 2` at 8 bits → `8'b11111101`; signed `-7 % 2` → `8'b11111111`.
  - `**` (8 bits): `0 ** -1` → all x; `0 ** 3` → 0; `2 ** -1` → 0; `(-1) ** -3` → −1; `(-1) ** -2` → 1; `1 ** -5` → 1; `3 ** 5` → 243; `7 ** 3` → 87.
  - Bitwise: `4'b10x1 & 4'b0011` → `4'b00x1`; `4'b10x1 | 4'b0100` → `4'b11x1`; `4'bz000 & 4'b1000` → `4'bx000`.
  - Reductions: `&4'b1x11` → x; `&4'b0x11` → 0; `|4'b0x00` → x; `|4'b1x00` → 1.
  - Equality: `4'b1x00 == 4'b1000` → x; `4'b1x00 === 4'b1x00` → 1; `!==` → 0.
  - Shifts: `8'sb10000000 >>> 2` signed → `8'b11100000`, unsigned → `8'b00100000`; an amount `2'bx1` → all x; amount 8 → all fill.
  - Mux: `cond = 1'bx`, `a = 4'b1100`, `b = 4'b1010` → `4'b1xx0`.
  - Resize: unsized `'hFFFF_FFFF` to 64 → all ones; sized `32'hFFFF_FFFF` → zero-extended; `'sd5` → `+5` sign-extended.
  - Strings: `"AB"` → `16'h4142`; `""` → `8'h00`.
  - `odin3_cval_print`: 8 → `8`; −3 → `-3`; `4'b10x0` → `4'b10x0`; 8-bit signed `0xF0` → `8'sb11110000`; the string `a"b` → `"a\"b"`.
  - `odin3_cval_to_number` of an `UNSIZED` value → payload width 0. A NUMBER built from it gives back the same bits with `is_unsized` from `odin3_ast_number_value`.
  - Hash equal for equal values and different for different flags.
  - Width 2^20 + 1 → `INVALID_ARG`.
  - OOM sweep over every allocating function.
- `test_gen_const_table.py`, with a fake `yosys` printing canned RTLIL: it parses `connect \y3 8'1x0z0000`, keeps batch order, is deterministic per seed, writes the version header, and refuses a `0.69` `-V`.

### Task 8: Expression typing (EL-13, EL-15/EL-16 types)
**Files:**
- Create `src/frontends/elab/ceval.h` (typing part) and `ctype.c`.
- Create `tests/unit/elab_test_tree.h`: static helpers that build parsed-form expression trees with the 2A builders, for tests that run without 2C. `et_ident`, `et_num` (via `odin3_ast_number_parse`), `et_unary`, `et_binary`, `et_ternary`, `et_concat`, `et_replicate`, `et_select`, `et_call`.
- Create `tests/unit/test_elab_ctype.c`.
- Modify `CMakeLists.txt` and `docs/specs/2026-10-09-2C-verilog-coverage.md` row E12 (§19).

**Consumes:** 2A store accessors, `odin3_diag`, Task 7.

**Produces:**
- `typedef struct odin3_etype { uint32_t width; bool is_signed; bool is_unsized; } odin3_etype;`
- `typedef struct odin3_ctype_host { odin3_status (*leaf)(void *user, odin3_ast_id node, odin3_etype *out); odin3_status (*constant)(void *user, odin3_ast_id node, int64_t *out); void (*report)(void *user, odin3_log_level level, odin3_loc loc, const char *msg); void *user; } odin3_ctype_host;`
  - `leaf` types an `IDENT`, a `HIER_NAME`, a `CALL` or a memory word, which the typer cannot type alone.
  - `constant` gives a replication count, part-select bound or `+:` width.
  - `report` routes diagnostics to the stage's counter.
- `typedef struct odin3_ctype odin3_ctype;` holds node → {self, final} in a `u64map`, reused across expressions.
- `odin3_status odin3_ctype_create(odin3_ast *ast, const odin3_ctype_host *host, odin3_ctype **out);`
- `void odin3_ctype_destroy(odin3_ctype *ty);`
- `odin3_status odin3_ctype_expr(odin3_ctype *ty, odin3_ast_id root, odin3_etype context);` (context width 0 = self-determined; an assignment passes `{L(lhs), true, false}`)
- `odin3_etype odin3_ctype_self(const odin3_ctype *ty, odin3_ast_id node);`
- `odin3_etype odin3_ctype_final(const odin3_ctype *ty, odin3_ast_id node);`

The two sweeps are §4.2's, each with an explicit stack: a post-order self sweep, then a pre-order context sweep. Diagnostics:
- `width N exceeds the limit of 1048576 bits` (located, `PARSE`);
- E12's warning `unsized constant in a concatenation is taken as N bits`, at the `NUMBER`.

**Tests (`test_elab_ctype.c`, hand-built trees):**

The host declares `a`, `b` `[7:0]` unsigned; `s` `signed [7:0]`; `t` `signed [3:0]`; `i` integer; `c` 1 bit; unsized `P = 'hFFFFFFFF`; `Q` `[31:0]` (kept unsized, EL-16); `R` `[35:0]` (sized); `f` returning `[3:0]`; `g` returning integer. Final (width, signed) for:

| Expression | Context | Final |
|---|---|---|
| `a + b` | 8 | `+` (8, u) |
| `a + b` | 9 | `+` (9, u), `a` and `b` (9, u) |
| E22: `(a + b) >> 1` | 8 | `+` (8, u) |
| E22: `(0 + a + b) >> 1` | 8 | both `+` (32, u), `>>` (32, u) |
| E23: `s + a` | — | (8, u) |
| E23: `s + t` | — | (8, s), `t` (8, s) |
| E8: `a < t` | — | `<` (1, u), operands (8, u) |
| E8: `s < t` | — | operands (8, s) |
| `t << a` | — | (4, s), `a` self (8, u) |
| `s >>> a` | — | (8, s) |
| `c ? a : t` | — | (8, u), `c` self (1, u) |
| `{a, t}` | — | (12, u) |
| `{a, 'h1}` | — | (40, u), plus the warning `x.v:1:12: warning: unsized constant in a concatenation is taken as 32 bits` |
| `{3{t}}` | — | (12, u) |
| `s[3:0]` | — | (4, u) |
| `s[2]` | — | (1, u) |
| `a[i +: 3]` | — | (3, u) |
| `$signed(a)` | — | (8, s) |
| `$unsigned(s)` | — | (8, u) |
| `t ** a` | — | (4, s), `a` self (8, u) |
| `&a`, `!a`, `a && b` | 8 | the operator (1, u), zero-extended to 8 |
| `f(a)` | — | (4, u) |
| `g(a)` | — | (32, s) |
| `P` | 64 | (64, u, unsized) |
| `Q` | 64 | (64, u, unsized) |
| `R` | 64 | (64, u) sized |

- `{1048577{1'b1}}` → `x.v:1:1: error: width 1048577 exceeds the limit of 1048576 bits`.
- A 10,000-term left-deep `+` chain types with the final width at every node, no recursion.
- OOM sweep.

### Task 9: Evaluator, interpreter and system functions (EL-12, EL-17, EL-18)
**Files:** Modify `ceval.h`; create `ceval.c`, `ceval_interp.c` and `tests/unit/test_elab_interp.c`; modify `CMakeLists.txt`.

**Consumes:** Tasks 7–8; 2C-1's `tests/unit/verilog_test.h` (`vt_parse`: one module text → parsed store, used to obtain `FUNCTION_DECL` trees).

**Produces:**
- `typedef enum odin3_csym_kind { ODIN3_CSYM_NONE = 0, ODIN3_CSYM_CONST, ODIN3_CSYM_SIGNAL, ODIN3_CSYM_FUNCTION, ODIN3_CSYM_TASK } odin3_csym_kind;`
- `typedef struct odin3_csym { uint8_t kind; odin3_cval value; odin3_etype type; odin3_ast_id decl; uint32_t name; } odin3_csym;`
- `typedef struct odin3_ceval_host { odin3_status (*lookup)(void *user, odin3_ast_id ref, odin3_csym *out); odin3_status (*callee)(void *user, odin3_ast_id call, odin3_csym *out); void (*report)(void *user, odin3_log_level level, odin3_loc loc, const char *msg); void *user; } odin3_ceval_host;`
- `typedef struct odin3_ceval_store { odin3_status (*read)(void *user, odin3_ast_id ref, odin3_cval *out, bool *known); odin3_status (*write)(void *user, odin3_ast_id lvalue, const odin3_cval *value); void *user; } odin3_ceval_store;` covers variables outside function frames (EL-40's initial state). Without a store, any such access is non-constant.
- `typedef struct odin3_ceval odin3_ceval;`
- `odin3_status odin3_ceval_create(odin3_ast *ast, const odin3_ceval_host *host, odin3_ceval **out);`
- `void odin3_ceval_destroy(odin3_ceval *ev);`
- `odin3_ctype *odin3_ceval_types(odin3_ceval *ev);`
- `typedef struct odin3_ceval_req { odin3_ast_id expr; odin3_etype context; const char *what; } odin3_ceval_req;`
- `odin3_status odin3_ceval_expr(odin3_ceval *ev, const odin3_ceval_req *req, odin3_arena *arena, odin3_cval *out);`
  - `OK`; or `PARSE` after the located `<what> is not constant (it reads 'x')`, a cap or an E20 error.
  - The result is copied into `arena`. The evaluation arena resets per call.
- `odin3_status odin3_ceval_try(odin3_ceval *ev, const odin3_ceval_req *req, odin3_arena *arena, odin3_cval *out, bool *is_const);`: no diagnostic for non-constant input; caps still report.
- `odin3_status odin3_ceval_call(odin3_ceval *ev, const odin3_ceval_req *req, odin3_arena *arena, odin3_cval *out);` interprets a `CALL` (§4.6).
- `odin3_status odin3_ceval_exec(odin3_ceval *ev, odin3_ast_id stmt, const odin3_ceval_store *store, bool *is_const);` runs one statement against `store` (Task 24).
- The interpreter is an explicit `odin3_ceval_frame { decl, cursor stack, locals }` stack. It covers §4.6's statements, `case` matching per EL-36, `$display`-family tasks ignored inside functions, and the non-constant reasons. The step budget counts statements plus expression nodes per top-level call.
- `$clog2`, `$signed` and `$unsigned` follow §4.7. Any other `$name` → `system function '$name' is not supported` (E20).

**Tests (`test_elab_interp.c`, sources through `vt_parse`, a test host binding module parameters):**
- Loop function `clog2`: `function integer clog2(input integer v); integer i; begin clog2 = 0; for (i = v - 1; i > 0; i = i >> 1) clog2 = clog2 + 1; end endfunction` → 1→0, 2→1, 3→2, 1024→10, 1025→11.
- `while`: `k = 0; while (k < n) k = k + 2; f = k;` → `f(5)` = 6.
- `casez (v) 4'b1???: f = 3; 4'b01??: f = 2; default: f = 0; endcase` → `f(4'b0110)` = 2, `f(4'b1000)` = 3, `f(4'b0001)` = 0.
- Recursion: `function automatic integer fact(input integer n); fact = (n <= 1) ? 1 : n * fact(n - 1); endfunction` → `fact(10)` = 3628800. With the call-depth hook at 8, `fact(9)` → `t.v:4:23: error: constant function recursion deeper than 8 in 'fact'`.
- Steps hook 1000 on `while (1) k = k + 1;` → `constant evaluation of 'f' exceeds 1000 steps`.
- Non-constant reasons, each exact:
  - `function 'f' is not a constant function: it reads module variable 'x'`
  - `… it calls task 't'`
  - `… it calls system task '$finish'`
  - `… it assigns 'r', which is not local`
- `$display` inside a function is ignored silently and the result is still computed.
- A result never assigned → all x.
- K7: a function declared after the parameter that calls it evaluates.
- `$clog2`: 0→0, 1→0, 2→1, 3→2, `32'h8000_0000`→31.
- `$clog2(w)` with `w` a signal → `$clog2 argument is not constant (it reads 'w')` (point (z)).
- `$random` → `system function '$random' is not supported` at the `CALL`.
- `odin3_ceval_try` on `a + 1` with `a` a signal → `is_const` false and zero log lines.
- `odin3_ceval_exec` with a store: `m[2] = 8'h12;` writes through the store.
- OOM sweep over `expr`, `call` and `exec`.

## PR 2D-3 — stage 1: hierarchy, parameters, specializations; the binder

### Task 10: Stage-1 context, the entry, options, module lookup, top selection (EL-2, EL-3, EL-6, EL-7)
**Files:**
- Create `src/frontends/verilog/elab1.h`, `elab1.c` (context, diagnostics helpers, `odin3_elab_stage1` and `odin3_elab_run`), `elab1_lookup.c` and `elab1_top.c`.
- Create `src/frontends/elab/elab2_stub.c` (point (l)).
- Create `tests/unit/elab_test.h` (static helpers, below) and `tests/unit/test_elab_stage1_lookup.c`.
- Delete `src/frontends/verilog/elab_stub.c`.
- Modify `src/frontends/verilog/elab.h` (point (a)) and `read_verilog.c`: the flags `--nolatches`, `--nomem2reg` and `--auto-blackbox` (any order, before the files); the usage line; the call.
- Modify `src/frontends/elab/elab.h` (stage APIs), `src/api/ast_abi.c` and `include/odin3/odin3.h` (`odin3_ast_get_library_units_from`, extending the v5 comment).
- Modify `docs/PASSES.md` (the `read_verilog` row gains the three flags), `docs/DESIGN.md` §4.0 (`--auto-blackbox`; the top rule unchanged) and `CMakeLists.txt`.

**Consumes:**
- 2C-2: `odin3_read_verilog_files`, `odin3_rv_opts`, `odin3_ast_library_units_from`, the pass's option parse.
- 2A: `odin3_ast_create(…, ODIN3_AST_FORM_ELABORATED, …)`, builders, `odin3_ast_finish`, `odin3_ast_check`, `odin3_srcman_buffer_info` (a unit's library).
- 2B-1 record (`top`, `top_at`, `params`); `odin3_pass_get_options`; `odin3_celltype_find` and `_get`; `odin3_module_prov` and `odin3_prov_get` (the run of a module made by an earlier read).

**Produces:**
- `src/frontends/verilog/elab.h`: `odin3_status odin3_elab_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_ast *parsed, const odin3_elab_opts *opts);` runs EL-2 steps 1–7.
- `src/frontends/elab/elab.h`:
  - `odin3_status odin3_elab_stage1(odin3_pass_ctx *ctx, odin3_ast *parsed, const odin3_elab_opts *opts, odin3_ast **elab_out);`
  - `odin3_status odin3_elab_stage2(odin3_pass_ctx *ctx, odin3_ast *elab, const odin3_elab_opts *opts);` (the stub until Task 17)
- `elab1.h`:
  - `typedef struct odin3_elab1 odin3_elab1;`
  - `typedef enum odin3_elab_def_kind { ODIN3_DEF_MODULE = 1, ODIN3_DEF_BLACKBOX, ODIN3_DEF_UDP, ODIN3_DEF_CONFIG } odin3_elab_def_kind;`
  - `typedef struct odin3_elab_def { uint8_t kind; bool library_unit; uint32_t name, library; odin3_ast_id node; uint32_t first_param, n_header_params, n_body_params, first_port, n_positions; } odin3_elab_def;`
  - `odin3_status odin3_elab1_create(odin3_pass_ctx *ctx, odin3_ast *parsed, const odin3_elab_opts *opts, odin3_elab1 **out);`
  - `void odin3_elab1_destroy(odin3_elab1 *e1);`
  - `void odin3_elab1_error(odin3_elab1 *e1, odin3_loc loc, const char *fmt, ...);` counts, and stops at the cap with `too many errors; elaboration stopped after 20`.
  - `void odin3_elab1_warn_once(odin3_elab1 *e1, odin3_ast_id parsed_node, const char *fmt, ...);` (point (v))
  - `void odin3_elab1_info(odin3_elab1 *e1, odin3_loc loc, const char *fmt, ...);`
  - `bool odin3_elab1_stopped(const odin3_elab1 *e1);`
  - `odin3_status odin3_elab1_collect_defs(odin3_elab1 *e1);` builds the definition table, EL-6's taken-name checks and Q7/S2.
  - `typedef enum odin3_elab_res_kind { ODIN3_RES_NONE = 0, ODIN3_RES_MODULE, ODIN3_RES_UDP, ODIN3_RES_CELLTYPE, ODIN3_RES_IMPLICIT_BLACKBOX } odin3_elab_res_kind;`
  - `typedef struct odin3_elab_res { uint8_t kind; uint32_t def; odin3_celltype_id type; } odin3_elab_res;`
  - `odin3_elab_res odin3_elab1_resolve(odin3_elab1 *e1, uint32_t name, uint32_t library);` follows EL-6 items 1–7, with the several-libraries info line once per name.
  - `odin3_status odin3_elab1_select_top(odin3_elab1 *e1, uint32_t *def);` is EL-7. Candidates count instantiations anywhere in a parsed body (divergence 20).
- ABI: `odin3_status odin3_ast_get_library_units_from(const odin3_ast *ast, uint32_t *index);` with the paragraph (0 = none; `INVALID_ARG` as the conventions say).
- `tests/unit/elab_test.h`:
  - `et_src { const char *path, *text; }`;
  - `et_stage1(const et_src *srcs, uint32_t n, const odin3_elab_opts *opts, et_result *out)` writes the sources into a fresh directory under the build tree, runs `odin3_read_verilog_files` and then `odin3_elab_stage1`, and fills `et_result { design, parsed, elab, status, dump (odin3_ast_dump of elab), log lines, errors, warnings }` through a capturing sink;
  - `et_free(et_result *r)`.

**Tests (`test_elab_stage1_lookup.c`; exact lines through the sink):**
- **Lookup order.**
  - `sub` in libraries `work` and `lib2`: an instance in a `work` module resolves `work.sub`.
  - `sub` only in `lib2` and `lib3` → info `module 'sub' found in libraries 'lib2', 'lib3'; using 'lib2.sub'`.
  - A `-v` library-unit module is used only when no design unit defines the name.
  - A `primitive p` → `p.v:1:11: error: user-defined primitive 'p' is not supported`, with or without an instance.
  - `config c; … endconfig` → `configuration 'c' is not supported`.
- **Cell types.**
  - After `read_techlib lib/vtr.o3lib`, `single_port_ram #(.ADDR_WIDTH(10)) r (…)` resolves to the cell type.
  - `$add u (…)` → `t.v:3:3: error: unresolved_module: module '$add' is not defined`.
  - `foo u (…)` → `unresolved_module: module 'foo' is not defined`.
  - With `--auto-blackbox`, `foo` resolves to an implicit black box, with point (w)'s info line.
- **Taken names.**
  - After `read_techlib lib/vtr.o3lib`, `module adder` → `a.v:1:8: error: module 'adder' has the name of cell type 'adder' registered by an earlier read (hard cell); rename the module`.
  - After a `read_blif` declaring black box `bb`, `module bb` → `… (black box); rename the module`.
  - Two `read_verilog` runs defining `m` → `duplicate_module: module 'm' is already defined by an earlier read (run 1)`.
- **Top.**
  - One candidate → info `top module 'work.t' chosen: the only module no other module instantiates`.
  - The corpus `neg_ambiguous_top` sources → `odin3: error: ambiguous_top: no top given and several modules are not instantiated: 'work.a', 'work.b'`.
  - `neg_no_top` → `no_top: no top given and every module is instantiated by another`.
  - No module → `no_top: no top given and no module is defined`.
  - `--top nope` → `odin3: error: unknown_top: 'nope' is not a defined module`.
  - A project top `nope` with `top_at` → located at the project line.
  - `--top` beats the project top.
  - `sub` in two libraries → `ambiguous_top: 'sub' is defined in several libraries: 'lib2.sub', 'lib3.sub'`; `--top lib3.sub` → OK.
  - Unreachable modules → info `not instantiated under the top, not elaborated: 'u1', 'u2'`.
  - A module instantiated only in an unselected generate branch is not a candidate.
- **ABI.** On a run with one `-v` file after two design files → index 2.
- **Entry.**
  - `read_verilog --nolatches t.v` passes `nolatches` true.
  - `read_verilog --bogus t.v` → `read_verilog: unknown option '--bogus'`, `INVALID_ARG`.
  - Before Task 17, a clean stage 1 is followed by the stub text and `PARSE`.
  - Debug `odin3_ast_check` FULL of the elaborated store passes.
- **OOM sweep** over stage 1 on a three-module design: `NO_MEMORY`, the parsed store intact, no elaborated store left, no leak.

### Task 11: Parameters, specializations, passes E/W, `defparam` and project push-down (EL-8, EL-9, EL-16)
**Files:**
- Create `src/frontends/verilog/elab1_params.c` (EL-16 typing of values, EL-8 precedence, cycles).
- Create `elab1_spec.c`: key bytes, `odin3_hash_bytes`, a `u64map` from hash to chain, names, the FIFO queue, the iterative-DFS acyclicity check, the depth cap.
- Create `elab1_expand.c` (the pass E skeleton: environments, parameters, instance and `defparam` collection, the decision log).
- Create `elab1_write.c` (the pass W skeleton: `MODULE`, the params `LIST` of `PARAM_DECL[localparam]{IN_HEADER}` with final values, `INSTANTIATION` names and named param `CONNECTION`s, the `UNIT` in BFS order).
- Create `tests/unit/test_elab_stage1_params.c`.

**Consumes:** Tasks 7–10; the project record's `odin3_proj_param { scope, name, value, type, at }`; `odin3_ast_number_parse` for project values.

**Produces:**
- `typedef enum odin3_elab_ovr_source { ODIN3_OVR_PROJECT = 1, ODIN3_OVR_NAMED, ODIN3_OVR_DEFPARAM, ODIN3_OVR_ORDERED } odin3_elab_ovr_source;` (precedence order)
- `typedef struct odin3_elab_override { uint32_t path; uint8_t source; uint32_t order; odin3_cval value; odin3_loc at; odin3_ast_id from; } odin3_elab_override;`: `path` is a strtab ID of `W` or `u2.W` relative to the target; `order` is the stream order (first `defparam` wins).
- `typedef struct odin3_elab_decision { odin3_ast_id node; int32_t value; } odin3_elab_decision;`
- `typedef struct odin3_elab_spec_req { uint32_t def; const odin3_elab_override *ovr; uint32_t n_ovr; uint32_t depth; } odin3_elab_spec_req;`
- `odin3_status odin3_elab1_params(odin3_elab1 *e1, const odin3_elab_spec_req *req, odin3_cval *values, odin3_elab_override *pending, uint32_t *n_pending);`
  - It evaluates the definition's `parameter`s lazily in declaration order, with overrides by precedence (EL-8) and converted per EL-16.
  - It returns the pending (deeper) overrides.
  - Errors are §9.1's (localparam, unknown, too many, mixed, depends on itself, not constant, P4).
- `odin3_status odin3_elab1_spec_get(odin3_elab1 *e1, const odin3_elab_spec_req *req, uint32_t *spec);` finds or creates; on create it pushes back and names per EL-9 and point (q).
- `odin3_status odin3_elab1_expand(odin3_elab1 *e1, uint32_t spec);`
- `odin3_status odin3_elab1_write(odin3_elab1 *e1, uint32_t spec);` replays the log in order. A log not consumed exactly is `ODIN3_ERR_CHECK` with `stage 1: decision log of 'm' not replayed (N of M entries)`, a 2D bug (§16 item 4).
- `odin3_status odin3_elab1_run_queue(odin3_elab1 *e1, uint32_t top_def);` (the §3.4 algorithm)
- Every elaborated node made from one parsed node copies its `{loc, end}`. A node stage 1 creates (a genvar `NUMBER`, a resolved `CONNECTION`, a folded bound) takes the range of the node that caused it (EL-4).

**Tests (`test_elab_stage1_params.c`; names and values asserted on the elaborated dump):** `module m #(parameter W = 1)`, instantiated from `top`:
- **Precedence (Review Focus 1).**
  - `m #(.W(5)) u1 (); defparam u1.W = 7;` → `u1` is `INSTANTIATION` `m$W=5`, plus `t.v:3:10: warning: defparam of 'u1.W' is overridden by the named parameter assignment of 'u1'`.
  - `defparam u3.W = 8; defparam u3.W = 9;` → `m$W=8`.
  - `m #(4) u4 (); defparam u4.W = 6;` → `m$W=6` (divergence 7).
  - Project `param top.u1.W 3` → `m$W=3`.
  - `m u5 ();` and `m #(.W(1)) u6 ();` → both `m`, one `MODULE` in the `UNIT`.
- **Sharing by resolved value.** On `parameter integer W`, `#(.W(8))` and `#(.W(32'sd8))` share one specialization. On an untyped `W`, `#(.W(8))` and `#(.W(4'd8))` → `m$W=8` and `m$W=4'b1000`.
- **Names.**
  - String `S = "ab"` overridden with `"x\"y"` → `m$S="x\"y"`.
  - `parameter signed [7:0] P` with −16 → `m$P=8'sb11110000`.
  - Untyped `'hFFFFFFFF` → `m$P=32'b11111111111111111111111111111111`.
  - A 200-bit override → `m$h` plus 16 hex digits.
  - A user module `m$W=5` already defined → the specialization is `m$W=5$2`.
  - A registered cell type named `m$W=6` → `m$W=6$2`.
- **Errors (exact).**
  - `#(.L(1))` on a `localparam L` → `'L' is a localparam of module 'm' and cannot be overridden`.
  - `#(.X(1))` → `module 'm' has no parameter 'X'`.
  - `#(1, 2, 3)` on two parameters → `too many parameter values for module 'm' (3 given, 2 parameters)`.
  - `#(1, .W(2))` → `parameter assignment of instance 'u' mixes ordered and named values`.
- **Dependencies.** `parameter A = B + 1, B = 2` → A = 3. `parameter A = B, B = A` → `parameter 'A' depends on itself` at `A`. `parameter A = w` → `parameter 'A' is not constant (it reads 'w')`.
- **EL-16 types.**
  - `parameter [7:0] P = 9'h1FF` → `8'hFF`, sized, unsigned.
  - `parameter signed P = 4'b1000` → signed, 4 bits.
  - `localparam [31:0] Q = 'hFFFFFFFF` → the elaborated `NUMBER` payload width is 0.
  - `localparam [35:0] R = 'hFFFFFFFF` → width 36.
  - `parameter real r = 1.0` → `parameter type 'real' is not supported`.
- **Push-down.**
  - `defparam u_mid.u_leaf.W = 4;` → `mid$u_leaf.W=4` and `leaf$W=4`.
  - `defparam top.u.W = 1;` written in a child → `defparam target 'top.u.W' does not name a parameter of an instance below this module`.
  - A `defparam` naming its own module's parameter → the same error.
  - Project `param top.u_wide.W 8'hFF` pushes down; `param top.u_wide.X 1` → `p.o3proj:4:1: error: param 'top.u_wide.X' does not name a parameter`.
  - The corpus `param_instance` values: its `ref.cmd` parameters equal.
- **Recursion and depth.**
  - `ping`/`pong` instantiating each other, with `--top ping` → `recursive instantiation of 'ping' (through 'pong')`.
  - A six-module distinct chain with the depth hook at 4 → `instance hierarchy deeper than 4 at 'u'; recursive instantiation of 'a4'?`.
  - The specialization hook at 2 with three specializations → its cap message.
- **Store shape.** `UNIT` named `top`, `top` first, then BFS order. Each `INSTANTIATION` has one named `CONNECTION` per parameter, with its final value.
- **Replay.** A hook dropping one log entry → `ODIN3_ERR_CHECK` with the exact text.
- OOM sweep.

### Task 12: Instances, connections, arrays, black boxes and cell types (EL-10, EL-11, EL-8 cell-type targets)
**Files:** Create `src/frontends/verilog/elab1_inst.c` and `tests/unit/test_elab_stage1_inst.c`; modify `elab1_expand.c` and `elab1_write.c`.

**Consumes:** Tasks 8–11. Typing of connection expressions uses a stage-1 `odin3_ctype_host` over pass E's environment.

**Produces:**
- `typedef struct odin3_elab_inst { odin3_ast_id instantiation, instance; uint32_t scope; odin3_elab_res target; uint32_t spec; } odin3_elab_inst;`
- `odin3_status odin3_elab1_connect(odin3_elab1 *e1, const odin3_elab_inst *inst);` resolves connections to one named `CONNECTION` per port in port order, applying EL-10's rules, errors and the unnamed-position warning.
- `odin3_status odin3_elab1_inst_array(odin3_elab1 *e1, const odin3_elab_inst *inst);` expands `u[hi:lo]` to `u[hi]`…`u[lo]` with `SELECT` slices (gates use P = 1).
- `odin3_status odin3_elab1_celltype_params(odin3_elab1 *e1, const odin3_elab_inst *inst);` checks overrides by name or order against the type and converts them by kind. Stage 2 infers the rest.
- `odin3_status odin3_elab1_blackbox(odin3_elab1 *e1, uint32_t def);`: `(* blackbox *)` modules get per-specialization ports and their body is ignored; implicit black boxes take the union of named ports, `inout`, with widths from the first instance.

**Tests (`test_elab_stage1_inst.c`):** with `module m(x, y, z)`:
- **Connections.**
  - `m u (a, , c)` → `CONNECTION`s `(x, a)`, `(y, —)`, `(z, c)`.
  - `.z(c), .x(a)` → port order.
  - `.y()` → no expression.
  - `.q(a)` → `module 'm' has no port 'q'`.
  - `.x(a), .x(b)` → `port 'x' of instance 'u' is connected twice`.
  - `m u (a, b, c, d)` → `instance 'u' of 'm' has 4 connections but 'm' has 3 ports`.
  - `m u (a, .y(b))` → `t.v:4:5: error: instance 'u' mixes ordered and named connections` at the `INSTANCE`.
- **Positions.** `module n(a, , b)` with `n u (x, y, z)` → warning `connection 2 of instance 'u' goes to an unnamed port position and is ignored`, then `(a, x)`, `(b, z)`. `u ()` on a port-less module → OK.
- **Arrays.** `m2 u[3:0] (.d(bus8), .q(q2))` with `d` 2 bits:
  - instances `u[3]`, `u[2]`, `u[1]`, `u[0]` in that order;
  - `u[3].d` = `bus8[7:6]`, `u[0].d` = `bus8[1:0]`, and the 2-bit `q2` goes whole to each;
  - a 3-bit connection → `connection of 'd' to instance array 'u' is 3 bits; expected 2 or 8`;
  - `and g[1:0] (y2, a2, b2)` → `g[1]` with `y2[1]`, `a2[1]`, `b2[1]`.
- **Outputs.** An output connected to `a + b` → `output port 'q' of instance 'u' must be connected to a net`; to `4'd3` → `output port 'q' of instance 'u' is connected to a constant`.
- **Black boxes.**
  - `(* blackbox *) module bb(input [3:0] a, output y); assign y = &a; endmodule` → info `body of black box 'bb' is ignored`, connections as written, no `MODULE` for `bb` in the `UNIT`.
  - A parameterized black box with two override values → two names per EL-9.
- **Cell types** (with `lib/vtr.o3lib`):
  - `single_port_ram #(.ADDR_WIDTH(10)) ram (…)` → the params `LIST` holds `(ADDR_WIDTH, 10)`.
  - `#(.FOO(1))` → `cell type 'single_port_ram' has no parameter 'FOO'`.
  - `defparam ram.DATA_WIDTH = 8;` → adds `(DATA_WIDTH, 8)`.
  - Project `param top.ram.ADDR_WIDTH 12` → wins.
  - `#(.ADDR_WIDTH(4'bx010))` → `value of parameter 'ADDR_WIDTH' of cell type 'single_port_ram' is not an integer`.
- **`--auto-blackbox`.**
  - `foo u1 (.a(x), .b(y)); foo u2 (.a(x2), .c(z));` → black box `foo` with ports `a b c`, all `inout`.
  - `foo u3 (x)` → `implicit black box 'foo' needs named connections`.
  - A 4-bit first `.a` and then an 8-bit one → point (w)'s error.
- The instance cap hook at 4 with an 8-element array → its cap message, fatal.
- OOM sweep.

### Task 13: The binder (EL-5)
**Files:** Create `src/frontends/elab/bind.c`, `src/frontends/elab/elab_internal.h` (binding part) and `tests/unit/test_elab_bind.c`; modify `tests/unit/elab_test_tree.h` (elaborated-store builders `eb_module`, `eb_net`, `eb_var`, `eb_localparam`, `eb_gen_block`, `eb_named_block`, `eb_function`, `eb_task`, `eb_assign`, `eb_always`).

**Consumes:** the 2A symbol table (`odin3_symtab_create`, `odin3_scope_new`, `odin3_symbol_new`, `_set_value`, `_set_unpacked`, `_add_assign`, `odin3_symtab_lookup`, `odin3_scope_child`); Task 7 (localparam values).

**Produces:**
- `typedef struct odin3_elab_binding { odin3_sym_id *sym_of; uint32_t n; odin3_u64map *scope_of; } odin3_elab_binding;`
- `odin3_status odin3_elab_bind(odin3_ast *elab, odin3_elab_binding *out);` is one pre-order sweep. Its errors are §2.5's located texts, capped at 20; it returns `PARSE` after reporting.
- `odin3_scope_id odin3_elab_scope_of(const odin3_elab_binding *b, odin3_ast_id node);` (`MODULE`, `GEN_BLOCK`, named `SEQ_BLOCK`, `FUNCTION_DECL`, `TASK_DECL`)
- `void odin3_elab_binding_free(odin3_elab_binding *b);`

**Tests (`test_elab_bind.c`):** hand-built elaborated stores, where module `t` has `wire [3:0] w; reg signed [7:0] r; localparam P` (a `NUMBER` with payload width 0, 32 ones), `GEN_BLOCK g` with indices 0 and 1 each holding `wire tt`, `SEQ_BLOCK blk` with `reg count`, function `f` and task `k`:
- **Symbols.** `w` is `NET` with packed `{3,0}`. `r` is `VAR` with `SIGNED`. `P` is `LOCALPARAM` with a 32-bit all-ones value, and its declarator's `NUMBER` has width 0.
- **Scopes.** `DESIGN` → `LIBRARY work` → `MODULE t` → `GENERATE g` (index 0), `GENERATE g` (index 1, on the same-name chain), `BLOCK blk`, `FUNCTION f`, `TASK k`.
- **Resolution.**
  - `w` read inside `g[0]` resolves outward to `t.w`.
  - `HIER_NAME g[1].tt` → g[1]'s `tt`; `blk.count` → `count`.
  - An `IDENT` used before its declaration resolves (divergence 1).
- **Assignments.** `assign w = …` adds `CONTINUOUS`; `r <= …` in an `always` adds `NONBLOCKING`; an assignment inside `f` is not recorded.
- **Errors (exact).**
  - `x` → `b.v:5:12: error: 'x' is not declared`.
  - `wire w; reg w;` → `'w' is already declared (first at b.v:2:13)`.
  - `u.w` → `hierarchical reference 'u.w' is not supported`.
  - `CALL k()` → `'k' is not a function`; `TASK_CALL f` → `'f' is not a task`.
  - A range bound `2147483648` → `width 2147483649 exceeds the limit of 1048576 bits`.
- 10,000 nested `GEN_BLOCK`s bind without recursion.
- OOM sweep: `NO_MEMORY`, the symtab destroyed with the store, no leak.

## PR 2D-4 — stage 1: generate, declarations, diagnostics; the handover on

### Task 14: Generate, scope names and references (EL-19, EL-20, EL-21, EL-22)
**Files:** Create `src/frontends/verilog/elab1_gen.c` and `tests/unit/test_elab_stage1_gen.c`; modify `elab1_expand.c`, `elab1_write.c` and `elab1.h`.

**Consumes:** Tasks 9–12 (constant conditions, the decision log, push-down for scoped `defparam`).

**Produces:**
- `typedef struct odin3_elab_scope { uint8_t kind; uint32_t name; int32_t index; bool has_index; uint32_t parent; uint32_t counter; uint32_t seen; } odin3_elab_scope;`: `seen` is the scope's set of `genblk*` names met so far, copied from the parent on entry.
- `odin3_status odin3_elab1_gen_for(odin3_elab1 *e1, odin3_ast_id node, uint32_t scope);`
- `odin3_status odin3_elab1_gen_if(odin3_elab1 *e1, odin3_ast_id node, uint32_t scope);`
- `odin3_status odin3_elab1_gen_case(odin3_elab1 *e1, odin3_ast_id node, uint32_t scope);`
- `odin3_status odin3_elab1_genblk_name(odin3_elab1 *e1, uint32_t scope, uint32_t counter, uint32_t *name);` is `label_genblks` (below).
- `odin3_status odin3_elab1_scope_ref(odin3_elab1 *e1, odin3_ast_id hier_name, uint32_t scope);` is G7, with E21 and the not-elaborated error.
- Decision-log entries:
  - `GEN_IF`: 0 (then), 1 (else) or −1 (none).
  - `GEN_CASE`: the item index or −1.
  - `GEN_FOR`: one entry with the iteration count n, then n entries with the genvar values.
- **Naming algorithm (EL-21, undetermined by the signatures).**
  - Per scope, a pre-order walk of its items.
  - Each `GEN_IF`, `GEN_FOR` or `GEN_CASE` increments the scope's counter c.
  - Each unnamed block of that construct is named `genblk<c>`, padded `genblk0<c>`, `genblk00<c>`, … while the name is in `seen`.
  - A user declaration whose name starts with `genblk` enters `seen` when the walk reaches it.
  - Entering a block starts a fresh counter at 0 with a copy of `seen`.
  - The else branch of `if … else if …` is a block of its own, named like the then-block.
  - Bare items are wrapped as blocks.

**Tests (`test_elab_stage1_gen.c`; the elaborated `GEN_BLOCK` names, indices and nesting):**
- **The review probe (Review Focus 5).** `localparam A = 0, B = 1; if (A) begin wire x; end else if (B) begin wire x; end if (B) assign y = a; genvar i; for (i = 0; i < 1; i = i + 1) begin wire w; end` gives:
  - `GEN_BLOCK genblk1` holding `GEN_BLOCK genblk1`, which declares `x` (path `genblk1.genblk1.x`);
  - `GEN_BLOCK genblk2` (the bare `if`);
  - `GEN_BLOCK genblk3` with index 0, declaring `w`.
- **User `genblk` names.** `wire genblk1;` after the first `if` → the block is still `genblk1`. Before it → `genblk01`.
- **Loops.**
  - `for (i = 3; i >= 0; i = i - 1) begin : g` → `g` indices 3, 2, 1, 0 in order.
  - `for (i = -1; i < 1; i = i + 1)` → `g[-1]`, `g[0]`.
  - `i = i * 1` from 0 → `generate loop of 'i' repeats the value 0`.
  - `integer k; for (k = 0; …)` → `'k' is not a genvar`.
  - Init `i`, step `j` → `generate loop must assign genvar 'i' in its initialisation and step`.
  - Nested loops on `i` → `genvar 'i' is already used by an enclosing loop`.
  - Condition reading wire `w` → `generate loop condition is not constant (it reads 'w')`.
  - The loop hook at 8 with `i < 100` → `loop exceeds 8 iterations`.
- **Case and if.**
  - `case (W) 1, 2: … default: … endcase` with W = 2 → item 0; with W = 5 → default; an item `2'b1x` never matches; no match and no default → nothing.
  - `if (1'bx)` → the else branch.
  - A `GEN_CASE` item reading a wire `w` → `case item in a generate case is not constant (it reads 'w')`.
- **References.**
  - `g[0].t` read in the module → a `HIER_NAME` with `NUMBER` index.
  - `g[5].t` with 5 not elaborated → `'g[5].t' does not exist (generate block 'g[5]' was not elaborated)`.
  - `u.w` through an instance → `hierarchical reference 'u.w' is not supported`.
- **Misplaced or clashing names.** `input p;` inside a loop body → `port declarations are not allowed in a generate block`. `wire g; for (…) begin : g` → `'g' is already declared (first at t.v:2:8)`.
- **Scoped push-down.** `defparam g[1].u.W = 2;` → `g[1].u` is `m$W=2` and `g[0].u` stays `m`.
- **Recursion through generate.** `module r #(parameter N = 3) (); if (N > 0) begin : s r #(N - 1) u (); end endmodule` → `MODULE`s `r`, `r$N=2`, `r$N=1`, `r$N=0`.
- A `parameter` inside a generate block cannot be overridden from outside (it is a `localparam`).
- An unselected branch with an error in it reports nothing.
- The elaborated store passes check FULL.
- OOM sweep.

### Task 15: Declarations, ports, implicit nets, blocks and subroutines (EL-4 normalisation, EL-23 stage 1, EL-24, EL-25)
**Files:**
- Create `src/frontends/verilog/elab1_decl.c` and `tests/unit/test_elab_stage1_decl.c`.
- Modify `elab1_expand.c` and `elab1_write.c`.
- Modify `docs/specs/2026-10-09-2A-ast-design.md` §4.6 (a pointer to EL-4 for the refined elaborated form, including the unsized `PARAM_DECL` value; §19).

**Consumes:** Tasks 9–14.

**Produces:**
- `odin3_status odin3_elab1_ports(odin3_elab1 *e1, uint32_t spec);` merges non-ANSI declarations into one `PORT_DECL{IN_HEADER}` per port in header order:
  - the `NET` field is never 0; `DT` is `reg`, `integer` or none; `SIGNED`;
  - `RANGE` bounds are `NUMBER`s;
  - empty positions are recorded for EL-10.
- `odin3_status odin3_elab1_decls(odin3_elab1 *e1, uint32_t scope, odin3_ast_id item);`: `NET_DECL`/`VAR_DECL` with `NUMBER` bounds, no `STRENGTH`/`DELAY`/`VECTORED`/`SCALARED`; initialisers kept.
- `odin3_status odin3_elab1_implicit(odin3_elab1 *e1, uint32_t spec);`: implicit nets per EL-25, first in the `MODULE`'s items, in first-use order, of the `` `default_nettype `` kind in effect.
- `uint8_t odin3_elab1_nettype_at(const odin3_elab1 *e1, odin3_ast_id module_node);`: the last `DIRECTIVE[default_nettype]` before the module in stream order, reset by `` `resetall ``.
- `odin3_status odin3_elab1_subroutine(odin3_elab1 *e1, odin3_ast_id decl, uint32_t scope);`: non-ANSI inputs move to the ports `LIST`, one declarator each; locals go to `LIST`.
- Expression normalisation in pass W:
  - genvar reads, `$clog2(…)`, `REPEAT` counts and `REPLICATE` counts become 32-bit signed `NUMBER`s;
  - `SELECT` `[m:l]` bounds and `+:`/`-:` widths become `NUMBER`s;
  - `ATTR` values are evaluated to `NUMBER` or `STRING` (`attribute value is not constant (it reads 'x')` otherwise);
  - unnamed blocks with declarations get `$unnamed_block$<k>` per module.

**Tests (`test_elab_stage1_decl.c`; dumps and exact lines):**
- **M1.**
  - `module m(q); output q; reg [3:0] q;` → `PORT_DECL[output]{IN_HEADER}` with `DT=reg` and `RANGE` `[3:0]`.
  - `output [3:0] q; reg [7:0] q;` → `port 'q' is declared with ranges [3:0] and [7:0]`.
  - `output signed q; reg [3:0] q;` → `SIGNED`.
- **Port lists.** `module m(a); endmodule` → `port 'a' is not declared`. `module m(a); input a; input b;` → `'b' is declared as a port but is not in the port list`.
- **Untyped ports.** ANSI `input a` → `NET` = 1 + `wire`, also under `` `default_nettype none ``.
- **M12.** `input reg a` → `warning: input 'a' is declared as a variable ('reg'); it is treated as a net`.
- **M6.** `module m(.x(a));` → `port expression in a module port list is not supported` at the `PORT_REF`.
- **Implicit nets (EL-25).**
  - `wire y; assign y = a & b;` → `NET_DECL[wire]` with declarators `a`, `b` first among the items, and two warnings `'a' is implicitly declared` and `'b' is implicitly declared` at their uses.
  - Under `` `default_nettype wand `` → `NET_DECL[wand]`.
  - Under `none` → `'a' is not declared (`default_nettype is none)`.
  - Under `tri0` → `net type 'tri0' is not supported` at the first use.
  - A use inside a generate block is declared at module scope.
  - An undeclared name used as a module or function is not an implicit net.
  - `parameter P = q;` → `'q' is not declared`.
- **`` `default_nettype `` placement.** Inside module 1 → `warning: `default_nettype inside a module takes effect at the next module`; module 2 uses it. `` `resetall `` resets to `wire`.
- **Unnamed blocks.** `always @* begin reg t; t = a; y = t; end` → `SEQ_BLOCK $unnamed_block$1` plus `warning: declaration in an unnamed block is a SystemVerilog extension; the block is named '$unnamed_block$1'`. The second one in the module is `$unnamed_block$2`; the next module restarts at 1.
- **Subroutines.** `function [3:0] f; input [3:0] a; input b; reg t; …` → ports `LIST(PORT_DECL a, PORT_DECL b)` and locals `LIST(VAR_DECL t)`.
- **Constant folds.**
  - `wire [$clog2(16)-1:0] w;` → `RANGE(NUMBER 3, NUMBER 0)`.
  - `repeat (N)` with N = 3 → `NUMBER 3`.
  - `{N{a}}` → a `NUMBER` count.
  - `{-1{a}}` → `replication count -1 is negative`.
  - `{2'bx1{a}}` → `replication count has x or z bits`.
  - `{0{a}}` alone → `zero-width concatenation`.
  - `wire [w:0] x;` with `w` a wire → `range bound is not constant (it reads 'w')`.
  - `a[P+1:P]` with `P` = 2 → `SELECT(…, NUMBER 3, NUMBER 2)`; `a[i +: W]` → the width a `NUMBER`.
  - `(* weight = P * 2 *)` → `ATTR` with `NUMBER`; `(* k = w *)` → `attribute value is not constant (it reads 'w')`.
- **Genvar reads.** A genvar read inside a loop body → a 32-bit signed `NUMBER` of the iteration's value.
- **M11 across kinds.** `wire x; reg x;` → `'x' is already declared (first at t.v:2:8)`.
- The elaborated store matches EL-4 for every kind in the test (check FULL).
- OOM sweep.

### Task 16: REJECT/IGNORE diagnostics, the error cap, the 2C handover on (EL-42, EL-43, EL-44; §14.4)
**Files:**
- Create `src/frontends/verilog/elab1_diag.c` and `tests/unit/test_elab_stage1_diag.c`.
- Modify `tools/parse-check/run-reject` (per-case pending, point (l)) and `tests/tools/test_run_reject.py`.
- Modify `tests/verilog/reject-2d/*.err` and `tests/verilog/ignore-2d/*.warn` only where 2C's text differs from §9 (conflict note 10).
- Modify `CMakeLists.txt`: `verilog_reject_2d` and `verilog_ignore_2d` now run.

**Consumes:** Tasks 10–15. The rows are §9.1 and §9.2. A4, T9 and T10 are stage 2's (Tasks 19 and 24).

**Produces:**
- `odin3_status odin3_elab1_reject(odin3_elab1 *e1, odin3_ast_id node);` covers the stage-1 REJECT rows at the nodes §9.1 names: L9, M11 (other than modules), N17, N18, E20, E21, T6, T12, T13, T14, T15, T16, T19, F4, K12, Q6, Q7, S2, N15/P4, F2.
- `odin3_status odin3_elab1_ignore(odin3_elab1 *e1, odin3_ast_id node);` covers the ten IGNORE rows' warnings, once per parsed node (point (v)).
- A mixed edge/level event list is rejected in stage 1, so the elaborated form holds none (EL-4): `edge and level events in one event list are not supported`. Task 21 keeps the same check for adapter stores.
- The cap: the 21st error is replaced by `too many errors; elaboration stopped after 20`, and stage 1 stops.
- Stage 1 resumes at the next item or module. Dependents of a failed item are skipped silently.
- `run-reject --elaborate`: per case **pass** / **pending** / **fail**. The summary is `pass N, pending P, fail F`. Exit 1 on any fail, 77 when P equals the case count, else 0.

**Tests (`test_elab_stage1_diag.c`):** one source per row, each asserting its exact located line from §9's tables:
- **REJECT texts.**
  - `real number in an expression is not supported`
  - `variable type 'real' is not supported`, also with `'time'` and `'realtime'`
  - `named event 'e' is not supported`
  - `system function '$random' is not supported`
  - `hierarchical reference 'u.w' is not supported`
  - `always block without an event control is not supported`
  - `event control inside an always body is not supported`
  - `'wait' is not supported`
  - `event trigger '->' is not supported`
  - `'fork ... join' is not supported`
  - `procedural 'assign' is not supported`, with `'deassign'`, `'force'` and `'release'`
  - `'disable' is not supported`
  - `'forever' is not supported`
  - `timing control in function 'f' is not supported`
  - `nonblocking assignment in function 'f' is not supported`
  - `switch-level primitive 'nmos' is not supported`
  - `'pullup' is not supported`
  - `net type 'trireg' is not supported`
  - `'while' is supported only in constant functions`
  - `edge and level events in one event list are not supported` (`@(posedge a or b)`)
- **IGNORE texts.**
  - `` `timescale is ignored ``
  - `` `celldefine is ignored `` (and the other four directives)
  - `specparam 'p' is ignored`
  - `drive strength is ignored`, `charge strength is ignored`, `net delay is ignored`, `'vectored' is ignored`, `'scalared' is ignored`
  - `delay on a continuous assignment is ignored`
  - `drive strength on a continuous assignment is ignored`
  - `delay is ignored` (`#5 x = 1;` and `x = #1 y;`)
  - `gate delay is ignored`, `gate drive strength is ignored`
  - `specify block is ignored`
  - `system task '$display' is ignored`
- **Once per node.** An IGNORE inside a 4-iteration generate loop warns once. One in a module specialized twice warns once.
- **Cap.** 25 `real` variables → 20 errors, then `too many errors; elaboration stopped after 20`, `PARSE`.
- **Resume.** Two errors in one module are both reported. An instance of a module whose parameter failed adds no second error.
- `test_run_reject.py`, over a fake `odin3`:
  - the stub text → pending;
  - all pending → 77;
  - a missing line without the stub → 1;
  - the summary line exact.
- CTests: `verilog_reject_2d` passes with exactly A4, T9 and T10 pending. `verilog_ignore_2d` passes with none pending.

## PR 2D-5 — stage 2: modules, wires, expressions, drivers, memories

### Task 17: Stage-2 context, modules, ports and wires, provenance, attributes, byte budget, `--elab-only`, `odin3_ast_elaborate` (EL-2 steps 4–7, EL-23, EL-24, EL-45, EL-46, EL-47)
**Files:**
- Create `src/frontends/elab/elab2.c` (context, module order, the steps (1)–(8) driver, summary) and `elab2_wires.c` (wires, ports, provenance, attributes).
- Create `src/api/elab_abi.c` and `tests/unit/test_elab_stage2_wires.c`.
- Delete `src/frontends/elab/elab2_stub.c`.
- Modify `elab_internal.h` and `elab.h`; `src/frontends/verilog/elab1.c` (`odin3_elab_run` steps 4–7).
- Modify `src/cli/main.c` (`--elab-only`) and `tests/cli/test_cli.sh` (`cli_elab_only`, `cli_elab_only_with_script`).
- Modify `include/odin3/odin3.h`: `odin3_elab_options` and `odin3_ast_elaborate`, extending the v5 comment.
- Modify `docs/PASSES.md` (the `read_verilog` row: elaborates; `--elab-only`), `docs/IR.md` IR-12 (module-relative `hier`, §10) and `CMakeLists.txt`.

**Consumes:** Task 2 (`odin3_port_spec` range/sign); Task 13; `odin3_module_create`, `odin3_module_add_port`, `odin3_wire_create`, `odin3_attr_set`, `odin3_prov_source`, `odin3_srcman_srcloc`, `odin3_symbol_set_ir`, `odin3_design_set_top`, `odin3_design_set_ast`, `odin3_design_drop_ast`.

**Produces:**
- Signals:
  - `typedef uint32_t odin3_sigbit;` is a net ID, or `ODIN3_SB_CONST | odin3_bit`, with `enum { ODIN3_SB_CONST = 1U << 31 };`.
  - `typedef struct odin3_sig { odin3_sigbit *bits; uint32_t width; bool is_signed; } odin3_sig;` (LSB first, in the stage-2 scratch arena)
- Context and module emission:
  - `typedef struct odin3_elab2 odin3_elab2;`
  - `odin3_status odin3_elab2_create(odin3_pass_ctx *ctx, odin3_ast *elab, const odin3_elab_opts *opts, odin3_elab2 **out);`
  - `void odin3_elab2_destroy(odin3_elab2 *e2);`
  - `odin3_status odin3_elab2_ports(odin3_elab2 *e2, odin3_ast_id module_node);` creates the module and its ports. It runs for every `MODULE` before any body.
  - `odin3_status odin3_elab2_module(odin3_elab2 *e2, odin3_ast_id module_node);` runs §6.6's steps (1)–(8). Steps not yet built log point (l)'s text and skip the construct.
  - `odin3_status odin3_elab2_wires(odin3_elab2 *e2, odin3_ast_id module_node);` is step (1), names per EL-23.
- Helpers:
  - `odin3_status odin3_elab2_const_net(odin3_elab2 *e2, odin3_bit bit, odin3_net_id *out);` gives the module's shared `$_CONST0_/1/X/Z_` net, created on first use (IR-4).
  - `typedef struct odin3_elab2_prov_req { odin3_ast_id node, call; uint32_t scope; } odin3_elab2_prov_req;`
  - `odin3_status odin3_elab2_prov(odin3_elab2 *e2, const odin3_elab2_prov_req *req, odin3_prov_id *out);` makes a SOURCE record per EL-45: locs = node, then call when inlined; `ast` = node; `hier` = `<spec name>/<scope path>`.
  - `odin3_status odin3_elab2_attrs(odin3_elab2 *e2, odin3_ast_id node, odin3_objref obj);` applies 2A §7: none → INT 1, `NUMBER` → BITS, `STRING` → STRING.
  - `void odin3_elab2_error(odin3_elab2 *e2, odin3_loc loc, const char *fmt, ...);` and `void odin3_elab2_warn(odin3_elab2 *e2, odin3_loc loc, const char *fmt, ...);` (cap 20; warn once per loc)
  - `odin3_status odin3_elab2_budget(odin3_elab2 *e2);` is EL-46's byte budget, checked at each module boundary and every 2^16 IR objects, with §11's per-object estimate.
- Real stage 2 replaces the stub: `odin3_elab_stage2` = bind → `ports` for all → `module` for each → `odin3_design_set_top` (the `UNIT`'s name) → the info summary `read_verilog: elaborated top 'T': M modules (S specializations), P processes, C cells, N nets, t1 + t2 ms`.
- ABI:
  - `typedef struct odin3_elab_options { uint32_t top; uint32_t top_at; bool nolatches, nomem2reg, auto_blackbox; uint32_t reserved[4]; } odin3_elab_options;`
  - `odin3_status odin3_ast_elaborate(odin3_design *design, odin3_ast *ast, const odin3_elab_options *opts);` (point (b)), with the paragraph. `reserved` must be zero, else `INVALID_ARG`.
- CLI: `odin3 --elab-only a.v b.v` ≡ `-p "read_verilog a.v b.v; stats"`. With a script it fails with point (w)'s text.

**Tests (`test_elab_stage2_wires.c`; `et_read` added to `elab_test.h` runs the whole pass):**
- **Wires.** `wire [0:3] w; wire [-1:2] n; reg signed [7:0] r; integer i;` → IR wires `w` `[0:3]` unsigned, `n` `[-1:2]`, `r` `[7:0]` signed, `i` `[31:0]` signed.
- **Scoped names (Review Focus 5).**
  - `g[0].t`, `blk.count`, `$unnamed_block$1.tmp`.
  - Task 14's probe gives wires `genblk1.genblk1.x` and `genblk3[0].w`.
  - `g[-1].t` exists.
- **Ports (IR change C).** `input signed [0:7] a` → port wire `[0:7]` signed. `input b` → `scalar`.
- **Ports before bodies.** A test hook observing body starts sees every module's port count final at the first body.
- **Provenance.**
  - Wire `w`'s record has `locs[0]` = its declarator (`t.v:2:15`–`2:16`), `ast` = the declarator's elaborated ID, `hier` = `top`.
  - `g[1].t` has `hier` `top/g[1]`.
  - A wire of `sub$W=8`'s `genblk1` has `sub$W=8/genblk1`.
- **Attributes.** `(* keep *) wire w;` → wire attribute `keep` INT 1; `(* src = "x" *)` → STRING; `(* init = 4'b1010 *)` → BITS.
- **Top.** `odin3_design_top` is `top`'s module. The summary line matches `^read_verilog: elaborated top 'top': 3 modules \(1 specializations\), 0 processes, [0-9]+ cells, [0-9]+ nets, [0-9.]+ \+ [0-9.]+ ms$`.
- **Budget.** The hook at 65536 bytes → `elaboration needs more than 65536 bytes (the budget is 60% of physical memory)`, `PARSE`, IR check passes.
- **ABI.**
  - A store built by hand through `odin3_design_new_ast(design, ODIN3_AST_FORM_ELABORATED, run, …)` and the ABI builders → `odin3_ast_elaborate` gives the module with its wires.
  - A parsed-form store → `INVALID_ARG` and `odin3_ast_elaborate: the store is not elaborated`.
  - Nonzero `reserved` → `INVALID_ARG`.
- **`ALWAYS` not built yet.** An `ALWAYS` (until Task 21) logs the stub text and gives `PARSE`.
- **Determinism.** Two runs give identical `write_rtlil`.
- **CLI.** `cli_elab_only` logs `stats:`. `cli_elab_only_with_script` fails with the text.
- OOM sweep over stage 2 of a three-module design: `NO_MEMORY`, IR check passes, no leak.

### Task 18: Expressions and selects to word cells (EL-30, EL-31, EL-32 reads)
**Files:** Create `src/frontends/elab/elab2_expr.c`, `elab2_select.c` and `tests/unit/test_elab_stage2_expr.c`; modify `elab_internal.h`.

**Consumes:** Tasks 1, 8, 9, 13, 17.

**Produces:**
- `typedef struct odin3_elab2_reads { odin3_status (*bit)(void *user, odin3_net_id net, odin3_sigbit *out); void *user; } odin3_elab2_reads;` is the `rv` substitution hook (Task 21); NULL outside processes.
- `typedef struct odin3_elab2_expr_req { odin3_ast_id expr; odin3_etype context; const odin3_net_id *dest; const odin3_elab2_reads *reads; } odin3_elab2_expr_req;`: `dest` is NULL, or `context.width` nets the outermost cell drives by construction (EL-28).
- `odin3_status odin3_elab2_expr(odin3_elab2 *e2, const odin3_elab2_expr_req *req, odin3_sig *out);` types the expression and emits it post-order. All-constant operands fold through `odin3_cval_op`.
- `typedef struct odin3_elab2_cell_req { const char *type; odin3_ast_id node; const odin3_sig *a, *b; uint32_t y_width; const odin3_net_id *dest; } odin3_elab2_cell_req;`
- `odin3_status odin3_elab2_cell(odin3_elab2 *e2, const odin3_elab2_cell_req *req, odin3_sig *y);` follows genrtlil's parameter rules: operand widths as generated, `*_SIGNED` = signed in context, the node's attributes, its provenance.
- `odin3_status odin3_elab2_to_nets(odin3_elab2 *e2, const odin3_sig *sig, odin3_net_id *nets);`
- `odin3_status odin3_elab2_extend(odin3_elab2 *e2, const odin3_sig *in, uint32_t width, bool is_signed, odin3_sig *out);` is wiring only, never a cell.
- `odin3_status odin3_elab2_select(odin3_elab2 *e2, const odin3_elab2_expr_req *req, odin3_sig *out);` handles constant selects, out-of-range x with the warning, and non-constant reads through `$shiftx` with genrtlil's offset computation.

**Tests (`test_elab_stage2_expr.c`; every cell's type and every parameter asserted). Signals:** 8-bit `a`, `b`, `y`; `signed [7:0] s`; `signed [3:0] t`; `[3:0] u`.
- **Assign by construction.** `y = a + b` → one `$add`: `A_WIDTH` 8, `B_WIDTH` 8, `Y_WIDTH` 8, `A_SIGNED` 0, `B_SIGNED` 0, its `Y` pins on `y`'s nets, no merge.
- **E22 (Review Focus 2).**
  - `y = (a + b) >> 1` → `$add` `Y_WIDTH` 8, then `$shr` with `A_WIDTH` 8, `B_WIDTH` 32 on the constant nets for 1, `Y_WIDTH` 8.
  - `y = (0 + a + b) >> 1` → two `$add`s with `Y_WIDTH` 32, then `$shr` `Y_WIDTH` 32, truncated to `y` by wiring.
- **E23.** `s + u` → `$add` with `A_SIGNED` 0, `B_SIGNED` 0, `A_WIDTH` 8, `B_WIDTH` 4. `s + t` → both `SIGNED` 1.
- **Unary and reduction rows.**
  - `-a` → `$neg` with `A_WIDTH` = the context width (extended by wiring first). `~a` → `$not`. `+a` → no cell.
  - `&a` → `$reduce_and` `Y_WIDTH` 1. `~&a` → `$reduce_and` + `$not` (1/1). `~^a` → `$reduce_xnor`.
  - `!a` → `$logic_not`. `a && b` → `$logic_and`.
- **Binary rows.**
  - `a ~^ b` → `$xnor`.
  - `a ** b` → `$pow` with `B_SIGNED` 0. `2 ** b` → `$shl` with A = constant 1.
  - `a / b` → `$div`; `a % b` → `$mod`.
  - `a < t` → `$lt` with both operands generated at 8 bits.
- **Ternary.**
  - `c ? a : b` (1-bit `c`) → `$mux` A=`b`, B=`a`, S=`c`, `WIDTH` 8.
  - With a 4-bit `c` → `$reduce_bool` then `$mux`.
  - `1 ? a : b` → no cell, `y` merged with `a`.
- **No-cell forms.** `{a, b}`, `$signed(a)`.
- **`===` and `!==`.**
  - `4'b1x00 === 4'b1x00` → constant 1, no cell.
  - `a4 !== 4'b1x00` → `$ne` with A = {`a4[3]`, `a4[1]`, `a4[0]`}, B = `3'b100`, plus `warning: '===' compares against x/z bits; they match any value`.
  - `a === b` → `$eq` plus `warning: '===' is treated as '==' (synthesis has no x or z)`.
  - `a4 === 4'bxxxx` → constant 1.
- **Constant selects.**
  - `w[2]` → the wire bit.
  - `w[9:6]` of `[7:0]` → bits 7, 6, x, x and `warning: range select [9:6] of 'w' is out of bounds; bits outside [7:0] are x`.
  - `w[0:3]` → `part-select [0:3] of 'w' is reversed relative to its declaration [7:0]`.
- **Non-constant reads.**
  - `w[i]` (`[7:0]`, integer `i`) → `$shiftx` with A = `w`, `Y_WIDTH` 1, B = `i`, `B_SIGNED` 1 (32-bit index).
  - `v[i]` of `[8:1]` → `$sub(i, 1)` (signed), then `$shiftx` `B_SIGNED` 1.
  - `x[i]` of ascending `[1:8]` → the two `$sub`s of EL-32.
  - `w[k -: 4]` → base `k − 3`.
  - E17: an out-of-range read is a `$shiftx` (x fill), never a `$shr`.
- **Attributes and folding.** `(* keep *)` on a `BINARY` → the `$add` node has `keep`. `y = 3 + 4` → `y`'s nets merged onto the constant nets for 7, no cell.
- **Depth.** A 2,000-term `&` chain → 1,999 `$and`s, no recursion.
- **E9 source text.** The bridge equivalence of the E9 file (`!==` against x) through Task 25's `elab-equiv` (quoted in the PR when Yosys 0.55 is present).
- OOM sweep.

### Task 19: Instances, gates, continuous assignments, drivers, wired nets, tri-state (EL-10 widths, EL-11, EL-26, EL-28, EL-29)
**Files:** Create `src/frontends/elab/elab2_inst.c`, `elab2_assign.c`, `elab2_drive.c` and `tests/unit/test_elab_stage2_drive.c`; modify `elab2.c` (steps 2, 4, 5, 7).

**Consumes:** Tasks 17–18; `odin3_net_merge`; `odin3_celltype_infer_params`; `odin3_celltype_declare_blackbox`; `odin3_node_create_connected`.

**Produces:**
- `typedef enum odin3_drv_kind { ODIN3_DRV_NONE = 0, ODIN3_DRV_PORT_IN, ODIN3_DRV_CONST, ODIN3_DRV_CELL, ODIN3_DRV_INSTANCE, ODIN3_DRV_GATE, ODIN3_DRV_ASSIGN, ODIN3_DRV_PROCESS, ODIN3_DRV_INOUT_PIN, ODIN3_DRV_TRIBUF } odin3_drv_kind;`
- `typedef struct odin3_drv { uint8_t kind; odin3_ast_id node; } odin3_drv;` (the table is a `vec` indexed by net ID)
- `typedef struct odin3_elab2_claim_req { const odin3_net_id *nets; uint32_t n; uint8_t kind; odin3_ast_id node; } odin3_elab2_claim_req;`
- `odin3_status odin3_elab2_claim(odin3_elab2 *e2, const odin3_elab2_claim_req *req, bool *granted);` checks every bit **before** attaching. It reports A4, `uwire`, `supply`, wired-net collection, the tri-state and `inout` rules, and leaves conflicting bits unattached.
- `odin3_status odin3_elab2_prescan(odin3_elab2 *e2, odin3_ast_id module_node);` is step (2)'s always-assigned bit set (also used by EL-27) and the wired-net procedural check.
- `odin3_status odin3_elab2_assign(odin3_elab2 *e2, odin3_ast_id net_assign);` (A1) assigns by construction or by merge, with the same-net no-op. A net declaration assignment (`wire w = e;`, N7) goes through it at the declaration.
- `supply0`/`supply1` nets are driven by the shared constant nets.
- Once per module, a wire with sinks and no driver warns `'w' is never driven`.
- `odin3_status odin3_elab2_instance(odin3_elab2 *e2, odin3_ast_id instance);` (I4 resizing, unconnected inputs to `$_CONSTX_`, `parameter:<NAME>` attributes, cell types with inferred parameters, black boxes declared per specialization)
- `odin3_status odin3_elab2_gate(odin3_elab2 *e2, odin3_ast_id gate_instance);` lowers gates as Yosys's `simplify` does:
  - `and`/`or`/`xor` → a left fold of 1-bit word cells;
  - `nand`/`nor`/`xnor` → the fold, then `$not`;
  - `buf`/`not` → one assignment per output;
  - `bufif*`/`notif*` → a ternary with z, i.e. `$tribuf`.
- `odin3_status odin3_elab2_wired(odin3_elab2 *e2, odin3_ast_id module_node);` is step (7): `$and`/`$or` chains in source order.

**Tests (`test_elab_stage2_drive.c`):**
- **Assignments.**
  - `assign y = a + b;` → `$add` `Y` on `y`.
  - `assign b2 = a;` → one net with alias count 1.
  - `assign c = 1'b1;` → `c` merged onto the `$_CONST1_` net.
  - `assign p = q; assign q = p;` → no error, plus `warning: 'p' and 'q' are connected only to each other and are never driven`.
  - `assign y[s] = d;` → `continuous assignment to 'y' with a non-constant index is not supported`.
  - `assign y[9:6] = d4;` → `warning: assignment to bits outside 'y' [7:0] is ignored`.
- **A4.**
  - `assign y = a; assign y = b;` → `t.v:4:10: error: 'y' has two drivers (also driven at t.v:3:10)`.
  - Whole then `assign y[3] = c;` → `'y[3]' has two drivers (also driven at t.v:3:10)`.
  - IR check passes in both cases.
- **Variables and special nets.**
  - `reg r; assign r = a;` → `warning: 'r' is a variable assigned by a continuous assignment; treated as a net`.
  - A `uwire u` with two `assign`s → `'u' is a uwire and has a second driver (first at t.v:3:10)`.
  - `supply0 s; assign s = a;` → `'s' has two drivers (also driven at t.v:2:9)`.
  - `supply1 v;` → `v`'s net is the `$_CONST1_` net.
  - `wire w = a & b;` (N7) → one `$and` driving `w`.
  - `wire w; assign y = w;` → `warning: 'w' is never driven`, once.
- **Wired nets.**
  - `wand w` with three whole-net drivers → two `$and` cells of the net's width, in source order.
  - `wor` with one driver of `w[3:0]` and one of `w[7:0]` → per-bit `$or`s.
  - One driver → no cell.
  - `always @* w = a;` on a `wand` → `'w' is a wired net (wand) and cannot be assigned in an always block`.
- **Tri-state (D3)**, on top-level `output y`:
  - `assign y = en ? d : 'bz;` → `$tribuf` with A=`d`, EN=`en`, Y=`y`, `WIDTH` 8.
  - `en ? 'bz : d` → EN = `$not(en)`.
  - `bufif0 (y1, d1, e1)` → EN via `$not`. `notif1` → A via `$not`.
  - Two `$tribuf`s on one port → OK. An `assign` plus a `$tribuf` → A4.
  - Internal `wire t; assign t = en ? d : 'bz;` → `tri-state driver on 't' is supported only on a top-level output or inout port (D3)`.
  - `assign y = 'bz;` alone → `y` on the `$_CONSTZ_` net.
- **Child `inout`s.** Two child `inout p` pins on net `bus` → OK. Adding `assign bus = a;` → `net 'bus' is driven by inout port 'p' of instance 'u' and by logic at t.v:5:10; internal tri-state nets are not supported (D3)`.
- **I4 resizing.**
  - An 8-bit input port fed 4 unsigned bits → zero-extended, plus `warning: port 'a' of instance 'u' (module 'sub') is 8 bits; the connection of 4 bits is extended`.
  - Fed a signed `t` → sign-extended.
  - A 12-bit connection → `… is truncated`.
  - A constant input → no warning.
  - An 8-bit output into a 12-bit net → the upper 4 bits driven by the port's MSB when the child declares `output signed`, else by 0.
  - A narrower output → the extra port bits on fresh nets.
  - An unconnected input → the `$_CONSTX_` net. An unconnected output → fresh nets.
- **Instances and cell types.**
  - Module instances carry `parameter:W` (BITS) for each overridden parameter.
  - `single_port_ram` (with `lib/vtr.o3lib`) and 10/8-bit address/data connections → `ADDR_WIDTH` 10, `DATA_WIDTH` 8 by inference.
  - A `(* blackbox *)` module → `odin3_celltype_declare_blackbox` once per specialization. A mismatch with a registered type → located `black box 'single_port_ram' does not match the registered cell type: <IR's why>`.
- **Gates.**
  - `and g (y1, a1, b1, c1);` → two `$and` (1/1/1), the second driving `y1`.
  - `nand` → plus `$not`. `xnor (y1, a1, b1)` → `$xor` + `$not`.
  - `buf (o1, o2, i1)` → both merged with `i1`.
- OOM sweep.

### Task 20: Memories — the mem2reg decision, `$mem` and its read ports (EL-27, EL-39 reads)
**Files:** Create `src/frontends/elab/elab2_mem.c` and `tests/unit/test_elab_stage2_mem.c`; modify `elab2.c` (steps 2 and 8) and `elab2_expr.c` (word reads).

**Consumes:** Tasks 17–19 (the pre-scan's per-array facts).

**Produces:**
- `typedef struct odin3_elab_mem { odin3_sym_id sym; bool to_regs; uint32_t width, size, abits; int32_t offset; bool flat; odin3_vec rd_ports, wr_ports; odin3_value init; } odin3_elab_mem;`
- `odin3_status odin3_elab2_mem_decide(odin3_elab2 *e2, odin3_ast_id module_node);` applies rules 1–6, `(* nomem2reg *)` and `--nomem2reg`, with the warning naming the triggering places.
- `typedef struct odin3_elab2_mem_rd { uint32_t mem; odin3_ast_id select; const odin3_elab2_reads *reads; } odin3_elab2_mem_rd;`
- `odin3_status odin3_elab2_mem_read(odin3_elab2 *e2, const odin3_elab2_mem_rd *rd, odin3_sig *data);` adds an asynchronous read port with the raw index, or the row-major address for a flattened array.
- `typedef struct odin3_elab2_mem_wr { uint32_t mem; const odin3_net_id *addr, *data, *en; uint32_t abits; odin3_net_id clk; bool posedge; odin3_ast_id stmt; } odin3_elab2_mem_wr;`
- `odin3_status odin3_elab2_mem_write(odin3_elab2 *e2, const odin3_elab2_mem_wr *wr);` (used from Task 24)
- `odin3_status odin3_elab2_mem_emit(odin3_elab2 *e2);` is step (8): one `$mem` per memory, ports in source order.
- Register-per-word arrays become wires `m[k]` (declared index). A constant-index access reads the word; out of range reads x with the warning.

**Tests (`test_elab_stage2_mem.c`):**
- **mem2reg rules.**
  - Rule 1: `wire [3:0] w [0:3]` → wires `w[0]`…`w[3]`, no warning. `(* mem2reg *)` on the array and on the module → registers.
  - Rule 2: written in `always @*` → registers plus `warning: replacing memory 'm' with list of registers; see t.v:12`. Written in `always @(posedge c or posedge r)` → registers.
  - Rule 3: read after a blocking write in the same block → registers.
  - Rule 4: `m[m[0]] <= x` and `{m[0], a} <= v` → registers.
  - Rule 5: constant-index writes only → registers.
  - `(* nomem2reg *)` exempts rules 2–5.
  - `--nomem2reg` with a net array assigned by `assign` → `array 'w' cannot be a memory (it is written in a combinational block) and --nomem2reg is set`.
- **Addressing (Review Focus 4)**, for a read-only array (no writes outside `initial`, so not rule 5):
  - `reg [7:0] m [1:4]; assign q = m[ra];` → one `$mem` with `MEMID` `"m"`, `WIDTH` 8, `SIZE` 4, `OFFSET` 1, `ABITS` = width(`ra`), `RD_PORTS` 1, `WR_PORTS` 0, `RD_CLK_ENABLE` `1'b0`, `RD_TRANSPARENT` `1'b0`, `RD_EN` on the constant-1 net, `RD_CLK` on the x net, and `RD_ADDR` pins **on `ra`'s nets** (raw index, no `$sub`).
  - `[4:1]` → `OFFSET` 1. `[-2:1]` → `OFFSET` −2.
  - `reg [7:0] m2 [0:3][1:2]` read `m2[i][j]` → `OFFSET` 0, `SIZE` 8, the address `(i − 0) × 2 + (j − 1)` as `$sub`/`$mul`/`$add` cells.
- **Registers per word.** `wire [3:0] w [0:3]; assign w[1] = a; assign y = w[1];` → `y` merged with `a`. `w[7]` → x plus the out-of-range warning. A variable-index read outside a process → the stub text until Task 23 (asserted here, flipped there).
- **Whole-array read.** `assign y = m;` → `'m' is a memory; select a word`.
- **Cap.** The memory-bits hook at 1024 with a 2048-bit array → `memory 'm' of 2048 bits exceeds the limit of 1024 bits`.
- OOM sweep.

## PR 2D-6 — stage 2: processes

### Task 21: The process generator core (EL-33 emission, EL-34, EL-35)
**Files:** Create `src/frontends/elab/pg.h`, `pg.c` and `tests/unit/test_elab_stage2_proc.c`; modify `elab2.c` (step 6; the `ALWAYS` stub path removed) and `CMakeLists.txt` (CTest `elab_proc_bridge_smoke` in the 0.55 block).

**Consumes:** Task 3 (`odin3_proc_body`, `odin3_proc_body_pack`, `$proc`), Tasks 17–19 (`odin3_elab2_expr` with `reads`, `odin3_elab2_claim`, `odin3_elab2_const_net`, `odin3_elab2_prov`), the binding.

**Produces (`pg.h`):**
- Bits: `typedef uint32_t odin3_pg_bit;` is a net ID, `ODIN3_PG_CONST | odin3_bit`, or `ODIN3_PG_ANY`, with `enum { ODIN3_PG_CONST = 1U << 31, ODIN3_PG_ANY = (1U << 31) | 4U };`.
- Lifetime and entry:
  - `typedef struct odin3_pg odin3_pg;` holds the builder vectors for cases, switches, actions, compares and syncs, the `lv`/`rv` maps with an undo log, the per-bit action-slot index for removals, and the frame stack.
  - `odin3_status odin3_pg_create(odin3_elab2 *e2, odin3_pg **out);` and `void odin3_pg_destroy(odin3_pg *pg);`
  - `odin3_status odin3_pg_always(odin3_pg *pg, odin3_ast_id always_node);` builds one block and creates one node, or none (§8.3 step 7).
- Statement and assignment machinery:
  - `odin3_status odin3_pg_stmt(odin3_pg *pg, odin3_ast_id stmt);` dispatches a statement through an explicit frame stack.
  - `typedef struct odin3_pg_assign_req { odin3_ast_id stmt, lhs; const odin3_sig *rhs; bool blocking; } odin3_pg_assign_req;`
  - `odin3_status odin3_pg_assign(odin3_pg *pg, const odin3_pg_assign_req *req);` (step 5)
  - `uint32_t odin3_pg_save(const odin3_pg *pg);` and `void odin3_pg_restore(odin3_pg *pg, uint32_t mark);` (the undo log)
  - `odin3_status odin3_pg_temp(odin3_pg *pg, uint32_t width, odin3_ast_id why, odin3_net_id *first);` makes fresh process-local nets.
  - `odin3_status odin3_pg_emit(odin3_pg *pg, odin3_ast_id prov_node, odin3_node_id *out);` runs step 7:
    - `O` pins and then `I` pins in net-ID order, constants as `I` pins on the shared constant nets;
    - the driver-table claim for every `O` net, dropping the actions of conflicting bits;
    - one `odin3_node_create_connected`.
- Syncs (EL-34): one per edge event, or one `ALWAYS`. Errors:
  - `posedge on 'x', which is N bits; a 1-bit signal is needed` (and `negedge`);
  - `edge and level events in one event list are not supported`.
- The T3 analysis names the first missing signal by name order.

**Tests (`test_elab_stage2_proc.c`; each `$proc` body asserted exactly: `I`/`O` pin nets in order, every case, action and sync):**
- **T7 (Review Focus 3).** `module t(input clk, a, b, output reg r); reg t; always @(posedge clk) begin t = a & b; r <= t; end endmodule` gives:
  - `I` = [`clk`, the `$and`'s Y];
  - `O` = [`r`, `t`, t0(`r`), t0(`t`)];
  - root actions `O3 ← I1`, `O2 ← I1`, in that order;
  - one `POSEDGE I0` sync with `O0 ← O2`, `O1 ← O3`.

  That is two outputs (§14.1). The `$and` has A=`a`, B=`b`.
- **T8.** `r1 <= a; r2 <= r1;` gives `I` = [`clk`, `a`], `O` = [`r1`, `r2`, t0(`r1`), t0(`r2`)], root `O2 ← I1`, `O3 ← O0` (the held value, §8.1 N6), and the sync updating both.
- **Held value.** `q <= q + 1` → the `$add` reads `q`'s net; root `O1 ← I1`; the sync `O0 ← O1`.
- **Combinational default mode.** `always @* y = a;` → root `t0(y) ← a`, one `ALWAYS` sync `y ← t0(y)`.
- **Constants.** `r <= 1'b0` → `I` holds the shared `$_CONST0_` net once; a second `<= 1'b0` in the same block reuses that pin.
- **Event lists.**
  - `@(posedge a or negedge b)` → syncs `POSEDGE I(a)`, `NEGEDGE I(b)` in order.
  - `@(a or b)`, `@(a, b)`, `@a`, `@*` → one `ALWAYS`.
  - `@(posedge w4)` → `posedge on 'w4', which is 4 bits; a 1-bit signal is needed`.
  - `@(posedge a or b)` → `edge and level events in one event list are not supported`.
  - `always @(a) y = a & b;` → `warning: incomplete sensitivity list: 'b' is read but not listed; the block is built as @*`, once.
- **Point (h).** `always @(posedge a) a <= ~a;` → a `POSEDGE` sync whose signal is `OUT|0`, accepted.
- **Net in a process.** `wire w; always @* w = a;` → `warning: 'w' is a net assigned in an always block; treated as a variable`.
- **Provenance.** The node's SOURCE record is its `ALWAYS`; each action, case and switch records its statement (EL-45).
- **No node.** `always @* ;` → no node, `odin3_module_proc_count` 0.
- **Different bits.** Two blocks writing `r[0]` and `r[1]` → two nodes, no error.
- **Not built yet.** An `ALWAYS` holding `if` (until Task 22) → the stub text.
- **Determinism.** Two runs give byte-identical `write_rtlil`.
- **Round trip.** `odin3_check_design` FULL on every result. `write_rtlil` → Yosys 0.55 `read_rtlil; proc` exits 0.
- CTest `elab_proc_bridge_smoke`: `elab-equiv` (Task 25's driver, cherry-picked here as a single file) on the T7, T8 and `q + 1` sources → exit 0.
- OOM sweep over `odin3_pg_always`: `NO_MEMORY`, no node created, no leak.

### Task 22: `if`, `case`, case temporaries, `full_case`/`parallel_case`, non-constant lvalue selects (EL-36, EL-32 writes)
**Files:**
- Create `src/frontends/elab/pg_case.c` and `tests/unit/test_elab_stage2_case.c`.
- Modify `docs/specs/2026-10-09-2A-ast-design.md` §7: the D1 warning is emitted by 2D, not 2E (EL-36, §19).

**Consumes:** Task 21.

**Produces:**
- `odin3_status odin3_pg_if(odin3_pg *pg, odin3_ast_id stmt);` and `odin3_status odin3_pg_case(odin3_pg *pg, odin3_ast_id stmt);` implement EL-36:
  - case temporaries t1 for bits the branches assign by blocking assignment, with `lv`/`rv` saved and restored around each case;
  - the added default case (`t1 ← rv(bits)`, or x under `full_case`);
  - `casez`/`casex` `ANY` positions;
  - constant folding of `if (0/1)` and of constant selectors;
  - the switch flags from `(* *)` and metacomment attributes;
  - D1's two warnings.
- `odin3_status odin3_pg_select_write(odin3_pg *pg, const odin3_pg_assign_req *req);` is the case method for `y[s] = d` and `y[b +: w] <= d`: a position temporary assigned once, one case per in-range position, no default.

**Tests (`test_elab_stage2_case.c`; exact bodies):**
- **`if`/`else`, default mode.** `always @* if (s) y = a; else y = b;` gives:
  - `O` = [`y`, t0, t1]; `I` = [`s`, `a`, `b`, `$_CONST1_` net];
  - root actions [`t0 ← t1`]; one switch on `I(s)`;
  - case [`1'1`] with [`t1 ← a`]; default case with [`t1 ← b`];
  - sync `ALWAYS` `y ← t0`.
- **Latch modes.** `always @* if (s) y = a;`:
  - default mode → the added default case holds [`t1 ← OUT(y)`], a latch path, plus root default `t0 ← y` removed by the final assignment;
  - `--nolatches` → no root default action, and the default case holds [`t1 ← 1'x`] with the `$_CONSTX_` net among `I`.
- **`case`.** `case (sel) 2'd0: y = a; 2'd1, 2'd2: y = b; default: y = c; endcase` → compares [`2'00`], [`2'01`, `2'10`], then default last. With `default` written first, it is still last.
- **Wildcards.** `casez` `2'b1?` → compare [`1`, `ANY`]. `casex` `2'bx1` → [`ANY`, `1`]. Plain `case` `2'bx1` → [x-net pin, `1`].
- **Case width.** `case (a4) 3'd1:` → the compare is 4 bits.
- **Flags.**
  - `(* full_case *) case (s) 1'b0: y = a; endcase` → switch flag `FULL_CASE`, an added default with [`t1 ← 1'x`], and `warning: 'full_case' is honoured: case values no item lists assign x`.
  - `// synopsys full_case parallel_case` → both flags and both warnings, the second `warning: 'parallel_case' is honoured: case items are not prioritised`.
- **Constants.** `if (1) y = a; else y = b;` → no switch. `case (2'd1)` with constant items → folded to that item.
- **Non-blocking in a branch.** `always @(posedge c) if (e) q <= d;` → root [`t0 ← q`], switch `e`, case [`1'1`] with [`t0 ← d`], and no t1 net.
- **Nesting.** `if (a) begin y = 1; if (b) y = 2; end` → nested switches, each with its own t1, and outer state restored after the inner one.
- **Variable-index write.** `always @(posedge c) y[s] <= d;` (`y` `[3:0]`, `s` 2 bits) → position temporary `p ← s`; a switch on `p` with cases `2'00`…`2'11`, each [`t0(y)[k] ← d`]; no default.
- **Indexed part-select write.** `y[b +: 2] <= d2` (`b` 2 bits) → cases for positions 0–2 only.
- **Ascending target.** An ascending `[0:3]` target maps positions by EL-23.
- **Bridge.** Each test source passes the Yosys 0.55 bridge in `--nolatches` mode (CTest `elab_case_bridge`, 0.55 block).
- OOM sweep.

### Task 23: Loops, functions and tasks, continuous-context calls, mem2reg variable reads (EL-37, EL-38, EL-27 reads)
**Files:** Create `src/frontends/elab/pg_loop.c`, `pg_call.c` and `tests/unit/test_elab_stage2_call.c`; modify `elab2_expr.c` (continuous-context calls) and `elab2_mem.c` (the register-per-word variable read).

**Consumes:** Tasks 9 (`odin3_ceval_try`, `odin3_ceval_call`), 20, 21 and 22.

**Produces:**
- `odin3_status odin3_pg_for(odin3_pg *pg, odin3_ast_id stmt);` and `odin3_status odin3_pg_repeat(odin3_pg *pg, odin3_ast_id stmt);` unroll with constant conditions, the loop frame, the final value, both caps, and point (k)'s starting count.
- `odin3_status odin3_pg_call(odin3_pg *pg, odin3_ast_id call, odin3_sig *result);` handles functions and task enables. It tries the interpreter first, then inlines through a call frame of fresh temporaries, copies outputs, and caps the depth.
- `odin3_status odin3_pg_comb_call(odin3_pg *pg, odin3_ast_id call, odin3_sig *result);` makes a combinational `$proc` (one `ALWAYS` sync) for a call in a continuous context.
- `odin3_status odin3_pg_comb_mem_read(odin3_pg *pg, odin3_ast_id select, odin3_sig *result);` makes a combinational `$proc` with a switch over the index, one case per word, default x.
- Inside a process, register-per-word reads and writes are inline cases.
- Loop variables are process-local unless read outside the block (EL-35, divergence 5).

**Tests (`test_elab_stage2_call.c`):**
- **Unrolled `for`.**
  - `always @* for (i = 0; i < 4; i = i + 1) y[i] = a[3 - i];` → four actions with constant selects. `i` gets no `O` pin.
  - If `i` is read outside the block, it is an output whose final action assigns 4.
- **Loop bounds and errors.**
  - `for (i = 3; i >= 0; i = i - 1)` with integer `i` → 4 iterations.
  - `reg [1:0] k; for (k = 0; k < 4; k = k + 1)` with the loop hook at 16 → `loop exceeds 16 iterations`.
  - `i < n` with `n` a signal → `for-loop condition is not constant (it reads 'n')`.
  - `i = 0;` in the body → `loop variable 'i' is assigned in the loop body`.
  - Init `i`, step `j` → `for-loop initialisation and step must assign the same variable`.
  - Two 20-iteration loops with the total hook at 32 → `unrolled loops exceed 32 iterations in this design`.
- **`repeat` and shared counters.** `repeat (3)` → the body three times. A shared `integer i` looping in two blocks → no T10.
- **Functions.**
  - `function [3:0] inc(input [3:0] v); inc = v + 1; endfunction` with `q <= inc(d);` → one `$add` reading the frame temporary of `v`; the temporaries are `O` pins with no sync action; no flip-flop for `v` or `inc`.
  - `inc(4'd3)` → folded to `4'd4`, no cell.
- **Tasks.**
  - `task tk(input x, output z); z = ~x; endtask always @* tk(p, y);` → `y` gets `$not(p)`.
  - `tk(p, a & b)` → `output argument 2 of task 'tk' must be a variable`.
  - Nested calls (K6, K11) and a call inside a loop → correct frames, equivalent through the bridge.
- **Continuous context.** `assign y = inc(d);` → one extra `$proc` with one `ALWAYS` sync, and `y`'s net driven by its `O` pin.
- **Inlined provenance.** Two call sites of `inc` give two distinct SOURCE records for their `$add`s, each with locs [the body's `BINARY`, the `CALL`] (EL-45).
- **Call errors.**
  - Recursion with non-constant arguments → `recursive call of 'f' cannot be inlined (its arguments are not constant)`.
  - `while` with non-constant arguments → `function 'f' can be called only with constant arguments (it uses 'while')`.
  - `inc(a, b)` → `function 'inc' has 1 inputs; 2 arguments given`.
- **mem2reg variable reads.**
  - `wire [3:0] w [0:3]; assign y = w[s];` → a `$proc` with a switch on `s`, cases `2'00`…`2'11` each [`t ← w[k]`], default [`t ← 4'x`]. This flips Task 20's stub assertion.
  - Inside a block, `m[s] <= d` on a register array → a case over `s` assigning one word each.
- OOM sweep.

### Task 24: Memory writes, initial values and `$readmem`, per-bit rules (EL-39, EL-40, EL-41)
**Files:**
- Create `src/frontends/elab/pg_init.c`, `tests/unit/test_elab_stage2_init.c`, `tests/unit/data/elab/readmem/{init.hex,init.bin,bad_addr.hex,bad_word.hex}` and `tests/elab/sink/sink.v` (a module using every construct of Tasks 17–24).
- Modify `pg.c` (memory write temporaries), `elab2_mem.c` (write ports, `INIT`), `elab2.c` (step 3) and `CMakeLists.txt` (CTest `elab_mem_bridge` in the 0.55 block).

**Consumes:** Task 9 (`odin3_ceval_exec` with an `odin3_ceval_store` over the module's variable bits), Tasks 20–23.

**Produces:**
- `odin3_status odin3_pg_mem_write(odin3_pg *pg, const odin3_pg_assign_req *req);` (EL-39) uses three process-local temporaries (`addr`, `data`, `en`), the root action `en ← 0`, and one write port per statement in source order.
- `odin3_status odin3_elab2_initials(odin3_elab2 *e2, odin3_ast_id module_node);` (EL-40) produces the three result kinds, the two D2 warnings and the ignored-value warning.
- `odin3_status odin3_elab2_readmem(odin3_elab2 *e2, odin3_ast_id call, const odin3_ceval_store *store);` parses as Yosys's `readmem` does, with point (y)'s path rule.
- The per-bit T9/T10 check at claim time, both places named from the symbol table's assignment lists.

**Tests (`test_elab_stage2_init.c`):**
- **Write ports (Review Focus 4).** `reg [7:0] m [1:4]; always @(posedge c) if (we) m[wa] <= d; assign q = m[ra];` gives one `$mem`:
  - `WR_PORTS` 1, `WR_CLK_ENABLE` `1'b1`, `WR_CLK_POLARITY` `1'b1`, `WR_CLK` = `c`;
  - `WR_ADDR`/`WR_DATA`/`WR_EN` = the `$proc`'s three `O`-pin temporaries, which have no sync action;
  - the body: root [`en ← 8'h00`], switch `we`, case [`addr ← wa`, `data ← d`, `en ← 8'hFF`];
  - `RD_ADDR` = `ra` raw, `OFFSET` 1.
- **Other bases.** `[4:1]` → `OFFSET` 1. `[-2:1]` → `OFFSET` −2.
- **Bridge.** CTest `elab_mem_bridge` runs the bridge on all three ranges, each with a write then a read at the same index → equivalent.
- **Partial and multiple writes.** `m[i][3:0] <= d4` → `en` `8'h0F`. Two write statements → two write ports in source order.
- **Initial values.**
  - `reg [3:0] r = 4'd5; always @(posedge c) r <= r + 1;` → wire `r` has `init` = `4'b0101`.
  - `initial begin m[1] = 8'h12; for (i = 2; i <= 4; i = i + 1) m[i] = i; end` → `$mem` `INIT` = words 1..4 = `12 02 03 04`.
  - `initial r2 = a;` (`a` an input) → `warning: initial statement is not constant and is ignored`.
  - `reg k; initial k = 1;` with nothing else driving `k` → `k`'s net merged onto `$_CONST1_`.
  - `assign w = a; initial w = 0;` → `warning: initial value of 'w' is ignored: 'w' is driven by a continuous assignment`; for an input port → `… driven by an input port`.
- **`$readmem`.**
  - `$readmemh("init.hex", m)`, the test running from another cwd, the file holding `// c`, `/* x */`, `@2` and `12 34` → `INIT` words 2, 3 = `12`, `34`, the rest x.
  - `$readmemb("init.bin", m, 1, 2)` → words 1–2.
  - `"nope.hex"` → `cannot open 'nope.hex' for $readmemh`.
  - `bad_addr.hex` → `bad address '@zz' in 'bad_addr.hex' line 3`.
  - `bad_word.hex` → `bad word 'g1' in 'bad_word.hex' line 1`.
  - `$readmemh` in an `always` → `$readmemh is supported only in an initial block`.
  - The readmem hook at 16 bytes → its cap message.
- **Per-bit rules.**
  - T9: `r = a; r <= b;` in one block → `t.v:4:5: error: 'r' is assigned with both '=' and '<=' (also at t.v:3:5)`; also across two blocks; an inlined task output counts as `=`.
  - T10: `always @(posedge c) r[2] <= a; always @(posedge c) r[2] <= b;` → `'r[2]' is assigned in two always blocks (also at t.v:3:22)`. `r[1]`/`r[2]` split across blocks → OK.
  - A4: `assign r = a; always @* r = b;` → `'r' has two drivers (also driven at t.v:3:10)`, and the IR has a single driver.
- **Handover closed.** `verilog_reject_2d` and `verilog_ignore_2d` pass with **0 pending**.
- **Sink.**
  - OOM over both stages of `tests/elab/sink/sink.v`: `NO_MEMORY` at every injection point, no leak, `odin3_check_design` FULL passes on what is left.
  - Two runs give byte-identical `write_rtlil`.

## PR 2D-7 — evidence (grows from 2D-3, closes last)

### Task 25: `elab-check`, the coverage rows, both latch modes' goldens and the Yosys 0.55 bridge (§14.2, §14.3)
**Files:**
- Create `tools/elab-check/elab-equiv` and `elab_equiv.py`. They are written first and cherry-picked into 2D-6 for Task 21's CTests.
- Create `tools/elab-check/elab-check.c` (C; links `odin3_core` like `odin3-parse-check`) and `tools/elab-check/micro-top` (Python, point (o)).
- Create `tests/elab/rows/rows.txt` and `tests/elab/rows/<row>_<slug>.v` for the 125 MUST and SHOULD rows, each starting `// <row>: <construct> — variants: …` and derived from 2C's coverage file minus its parse-only variants.
- Create `tests/elab/rows/<row>.elab`. The 54 rows of §14.2's list (point (z)) are hand-written, first line `# hand-written`: M1, M2, M12, P1–P6, P8, N2, N7–N13, E8, E9, E12, E15, E16, E22, E23, G1–G7, C4–C9, T7, T18, F1, K1–K11, I3, I6. The rest are generated with `--write` and reviewed once in the diff.
- Create `tests/elab/rows/<row>.il` (default mode) and `<row>.nolatches.il` for every row with a combinational `always`.
- Create `tests/elab/equiv_expect_fail.txt`, `tests/tools/test_elab_equiv.py` and `tests/tools/test_elab_rows.py`.
- Modify `CMakeLists.txt`: the `ODIN3_YOSYS055` block (point (p)); CTests `elab_coverage` (label `elab`) and `elab_coverage_equiv` (0.55 block).

**Consumes:** every earlier task; `odin3_ast_dump`; `odin3_rtlil_text`; `tools/equiv-check/equiv-check`.

**Produces:**
- `elab-check [--write] [--nolatches] [--top T] <file>…`. For each file, run from its own directory under its basename:
  1. `read_verilog` (both stages, Debug checks on);
  2. `odin3_ast_check` FULL and `odin3_check_design` FULL;
  3. compare `odin3_ast_dump(elab)` with `<row>.elab` (`--write` refuses a `# hand-written` file);
  4. compare `odin3_rtlil_text` with `<row>.il`, or with `<row>.nolatches.il` under `--nolatches`.

  Exit 0 if all pass, 1 if any differs, 2 for usage or I/O errors.
- `elab-equiv --odin3 O --yosys Y --equiv-check E [--top T] [--nolatches] [--mem-cap B] [--keep DIR] [--allow-version V] <file>`:
  - **A** = `O -p "read_verilog [--nolatches] F; write_rtlil a.il"`, then `Y -p "read_rtlil a.il; hierarchy -top T; proc; flatten; opt; memory -nomap; opt; memory_map; opt -full; techmap; opt -fast; abc -g AND,NAND,OR,NOR,XOR,XNOR,MUX; opt_clean; write_blif -gates -undef + unconn a.blif"`.
  - **B** = the same Yosys script with `read_verilog -sv -nolatches F` in place of `read_rtlil a.il`.
  - `Y -p "read_rtlil a.il; write_rtlil b.il"` must exit 0.
  - Then `E a.blif b.blif`.
  - Exit 0 equivalent, 1 not, 2 error.
  - Every child runs under point (n)'s cap.
  - It refuses a Yosys whose `-V` lacks `0.55` unless `--allow-version` names it (the nightly drift job only).

**Tests:**
- `test_elab_equiv.py`, over fake `odin3`, `yosys` and `equiv-check` scripts that log their argv:
  - the two Yosys command strings are exactly the ones above;
  - the exit-code mapping;
  - a `0.69` version refused and accepted with `--allow-version 0.69`;
  - the `systemd-run` wrapper in argv when available.
- `test_elab_rows.py`:
  - `rows.txt` lists exactly the 125 IDs whose verdict in the audit's §4 tables starts with `MUST` or `SHOULD`, plus N15's `uwire` SHOULD, each once;
  - every file exists with its header;
  - exactly the 54 hand-written rows have an `.elab` starting `# hand-written`;
  - every row flagged `comb` has `.nolatches.il`.
- `elab_coverage`: green on all 125 rows, smallest first, in both modes.
- `elab_coverage_equiv`: green except the rows in `equiv_expect_fail.txt`, each with its reason (multi-clock rows rely on their goldens, §14.2).

### Task 26: Project corpus, the handover close-out and the micro sweeps (§14.4, §14.5)
**Files:**
- Create `tools/elab-check/elab-sweep` (Python): smallest first, point (n)'s caps, expect-fail lists, modes `--elab-only` and `--equiv`.
- Create `tests/elab/micro_expect_fail.txt` and `tests/tools/test_elab_sweep.py`.
- Modify `tools/project-fixtures/project_fixtures.py`: a `--with-frontend` mode that runs `elab-equiv` on the positive phase-2 cases and kind/location/name checks on the negative ones.
- Modify `CMakeLists.txt`: CTests `verilog_micro_elab`, `elab_equiv_micro` (0.55 block) and `project_corpus_frontend`.

**Consumes:** Task 25; 2B-3's `project-fixtures compare`; the Parmys golden tree (read-only).

**Produces:**
- `elab-sweep --odin3 O [--equiv --yosys Y --equiv-check E] [--expect-fail F] [--jobs J] [--mem-cap B] <dir or file>…`:
  - It collects `.v` files: a multi-file micro is run through its includer, and a header-only `.vh` never alone.
  - It sorts them by size and runs each under `--elab-only` or the bridge. `--top` comes from `micro-top` when a golden exists.
  - It prints one line per file and a summary.
  - An unlisted failure or a listed pass fails the run. A listed entry is `<relative path> <one-line reason>`.
- `micro_expect_fail.txt` lists only:
  - the Parmys-rejected micros (PHASE2 #4, the 2C list's entries);
  - the three splicing micros (PHASE2 #11);
  - files needing a header the tree lacks.

  Each line has its reason.

**Tests:**
- `test_elab_sweep.py`, over fakes:
  - ascending size order;
  - `--jobs 2 --mem-cap 10G` refused against a fake `/proc/meminfo` of 24 GB (2 × 10 > 14.4);
  - a listed pass → 1, an unlisted failure → 1, a listed failure → 0;
  - `--top` taken from a fake golden's first `.model`;
  - a child killed by its cap reported `out of memory (cap 4G)`.
- `verilog_micro_elab` (539 files): green with the list.
- `elab_equiv_micro`: green with the list (local; its run log quoted in the PR).
- `project_corpus_frontend`:
  - `auto_top`, `param_instance`, `param_string`, `top_param`, `single_file` and `multi_dir` are equivalent to their `ref.blif` through the bridge;
  - `neg_unresolved_module`, `neg_unknown_top`, `neg_ambiguous_top` and `neg_no_top` give their kind, location and names;
  - a case where the fixture's 0.69 `ref.cmd` and 0.55 disagree is reported `oracle drift`, not failed.
- `verilog_reject_2d` and `verilog_ignore_2d` are re-run here with 0 pending.

### Task 27: VTR sweeps, `bench_elab`, the PHASE2 entry (§14.5, §15)
**Files:**
- Create `tests/bench/bench_elab.c` (Release).
- Modify `CMakeLists.txt`: CTests `verilog_vtr_elab` when `VTR_ROOT` is set; `elab_equiv_vtr` with label `nightly`, not in the default set; `bench_elab_smoke`.
- Modify `.github/workflows/nightly.yml`: the VTR bridge, the 5,000-seed generator run and the 0.69 drift job, as commented steps until a runner with VTR exists.
- Modify `docs/PHASE2.md`: 2D's entry, the next free number.

**Consumes:** Tasks 25–26; `odin3_ast_bytes_reserved`; the IR store ends.

**Produces:**
- `bench_elab --one <file>` prints one line for one file:
  - bytes and parse time;
  - stage-1, binding and stage-2 times, separately;
  - parsed and elaborated node counts and `odin3_ast_bytes_reserved` of each;
  - modules, specializations, cells, `$proc` nodes, `$mem` cells, nets, wires and pins;
  - each EL-46 estimate term and the estimated total;
  - peak RSS (`getrusage`).
- `bench_elab --set <file>…` runs each file in its own capped child via `elab-sweep`, smallest first:
  - the 31 files of `$VTR_ROOT/vtr_flow/benchmarks/verilog/*.v` first, `mcml.v` last of them;
  - then the 33 `koios/*.v`, `lenet.v` last, run alone under the 8 GiB cap;
  - then the §15 targets checked and printed `ok`/`MISSED`.
- `verilog_vtr_elab` runs the same 64 files under `--elab-only`, smallest first, with the expect-fail list.

**Tests:**
- `bench_elab_smoke` runs `--one` on two micros and checks every column is present.
- `verilog_vtr_elab`: green locally on all 64 files, with the run log quoted in the PR.
- Bench on the dev machine, targets from §15:
  - `mcml.v` stage 1 + stage 2 ≤ 0.5 s and peak RSS ≤ 200 MB;
  - `lenet.v` ≤ 10 s and ≤ 2.0 GB;
  - the 31 VTR files ≤ 3 s total.

  A missed target is investigated and reported with the estimate terms, never relaxed.
- `elab_equiv_vtr`: run once locally, smallest first (memory rule), with its summary quoted in the PR; `lenet.v`'s Yosys side is reported with its peak RSS.
- The PHASE2 entry records:
  - the bench output verbatim and the two sweeps' summaries;
  - the divergence list §17 by reference;
  - the Parmys exceptions added to PHASE2 #4;
  - this plan's points (a)–(z) as agent defaults;
  - the x preview table of Task 29;
  - the three IR changes as "landed, for Peter's review".

### Task 28: Crash fuzzer, differential generator, constant and cell differentials (§14.5)
**Files:**
- Create `tests/fuzz/fuzz_elab.c`, `tools/elab-check/elab-diff` (Python), `tools/elab-check/const-diff` (Python), `tests/unit/test_elab_cellsim.c` and `tests/tools/test_elab_diff.py`.
- Modify `tools/verilog-gen/verilog_gen.py` (`--elab`) and its test.
- Modify `CMakeLists.txt`:
  - `fuzz_elab` and the 10 s smoke `fuzz_elab_smoke` in the `ODIN3_FUZZ` block;
  - `elab_gen_vs_yosys` with 200 seeds and `elab_const_diff` with 500 expressions, in the 0.55 block;
  - nightly labels for 5,000 seeds, 10,000 expressions and the 0.69 drift.

**Consumes:** Tasks 7 and 25; 2C-2's generator and fuzz block; `odin3_elab_test_set_limits`; the cell `simulate` hooks.

**Produces:**
- `fuzz_elab.c` (`LLVMFuzzerTestOneInput`): bytes → preprocess → parse → elaborate, with every cap lowered. Invariants, each violation a `__builtin_trap()` in `tests/` only:
  - the status is `OK`, `PARSE` (1–20 located errors, no `INVALID_ARG` logged) or injected `NO_MEMORY`;
  - on `OK`, IR check FULL passes;
  - no leak.

  Seeds are `tests/elab/rows/*.v` and the micros' `common/` directory.
- `verilog_gen.py --elab` adds §14.5's traps, each a counted feature:
  - random-width and random-sign expression trees into random-width targets;
  - unsized parameters and literals;
  - `===` against x patterns;
  - `#()` and `defparam`;
  - generate loops and conditionals;
  - `always` blocks mixing `=` and `<=`;
  - `case`/`casez`, loops, functions and tasks;
  - memories with non-zero low indices.
- `elab-diff --seeds A..B [--yosys-069 Y2]` runs `elab-equiv` per seed. A non-equivalence fails and keeps the seed and file. With `--yosys-069`, a file where 0.55 and 0.69 disagree is listed as `oracle drift`, not failed.
- `const-diff --count N --seed S` generates random constant expressions over Task 7's ops and compares `odin3_cval_op` through a tiny driver against Yosys 0.55's values.

**Tests:**
- `test_elab_diff.py`:
  - deterministic per seed;
  - each trap feature appears in at least one of seeds 1–50;
  - drift is classified, not failed.
- `elab_gen_vs_yosys` passes on 200 seeds. A local 5,000-seed run's summary is quoted in the PR.
- `elab_const_diff` passes on 500 expressions.
- `fuzz_elab_smoke` runs 10 s with no crash (clang).
- `test_elab_cellsim`: for every word cell with a `simulate` hook (the 1E set plus Task 1's seven), 1,000 random 0/1 cases at widths 1, 7, 64 and 65, with random signs. `odin3_cval_op` and the hook agree bit for bit.

### Task 29: `equiv-check --x-report` (2H-owned, §14.6 item 4) and the report-only Parmys preview
**Files:**
- Modify `tools/equiv-check/equiv_check.py` and `tests/tools/test_equiv_check.py`.
- Create the fixtures `tests/tools/fixtures/xreport/{a,b_unconn,c_logic,d_latch}.blif` and `tools/elab-check/parmys-preview` (Python).
- Modify `docs/ORACLES.md` (the mode and the four outcomes).

**Consumes:** `tools/equiv-check` (ABC `cec`/`dsec`), `tools/netlist-compare`, Task 25's bridge A side, the Parmys goldens (read-only).

**Produces:**
- `equiv-check --x-report a.blif b.blif`, an algorithm the existing interface leaves open:
  1. Run the normal check. Equivalent → exit 0.
  2. Otherwise, in each netlist, collect the x sources: the net `unconn` (a zero-row `.names`) and any net driven by an x constant.
  3. Walk each primary output's and each latch input's transitive fan-in, crossing latches (the sequential cone), with an explicit stack.
  4. Remove from both netlists every output and latch input whose cone reaches an x source in **either** netlist.
  5. Rerun `cec`/`dsec` on the rest.
  6. Report: equivalent on the rest → exit **3** with `equivalent outside x-cones; removed: <name>, …` (names sorted); still differing → exit 1 with the same list. Exit 2 on error.
- `parmys-preview --sweep <list> --out work/2D-parmys-preview.tsv` runs, per micro and VTR file (smallest first, capped):
  - bridge side A, then `write_blif -gates -undef + unconn`;
  - against `<leaf>.parmys.blif`;
  - `netlist-compare`, then `equiv-check --x-report`;
  - each file goes in one of four outcomes: **identical**, **equivalent**, **equivalent outside x-cones** (with the removed list), **differs**.

  It is not a CTest and never gates a 2D PR (point (m)). Its summary goes into PHASE2 (Task 27) for Peter's §20 item 13 ruling.

**Tests (`test_equiv_check.py`):**
- `a` against itself → 0.
- `a` against `b_unconn`, which differs only in output `y` whose cone reaches `unconn` → exit 3 and the line `equivalent outside x-cones; removed: y`.
- `a` against `c_logic`, differing in `z` with no x in its cone → exit 1.
- `a` against `d_latch`, a latch input `q_next` whose cone reaches `unconn` → exit 3 with `removed: q_next`.
- A malformed file → 2.
- The existing modes are unchanged (the current tests pass).
