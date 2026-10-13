# 2D Elaboration — Implementation Plan (v3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

v2 applies the Opus plan review (`2D-plan-review`: 3 critical, 14 important, 24 minor) and the controller's rulings on it:
- C1: EL-39's x defaults, made in the spec (v4).
- C2: CMake block and bridge ownership.
- C3: unhandled items get a located "not available yet" error.
- I1: stay on the unreleased ABI v4.
- I7: reductions take the context width.
- I12: a `sweep` label.
- I13: `--x-report` stays in 2H.

The v1 plan had 29 tasks; v2 has 28.

v3 applies the re-review (R1–R6):
- R1: stage-2 pending entries land with their constructs: assignments in Task 18, instances, gates and A4 in Task 19, and the error-cap test moves to Task 19.
- R2: Task 24's memory-write body follows the case-temporary removal rule, with the spec's EL-39 wording to match.
- R3: the `duplicate_module` test moves to Task 17.
- R4: `write_rtlil` declares named non-wire nets.
- R5: a `sweep` workflow preset builds Release itself (superseded: point (n) as amended has no workflow preset for sweeps).
- R6: the stage-2 type cache lists its types and supplies `$proc`'s ID.

**Goal:** Turn the parsed store of one `read_verilog` run into word-level IR with processes. Stage 1 (Verilog only) builds the elaborated store: module lookup, top selection, parameters with Yosys's precedence, specializations, `generate`, constant functions, implicit nets, non-ANSI port merging and the REJECT/IGNORE diagnostics. Stage 2 (shared with the Phase 5 slang adapter) binds that store and emits wires, ports, word cells, instances, drivers, memories, `$proc` process cells, inlined functions and initial values. The IR changes A (nine registry cells), B (the `$proc` cell) and C (port range and sign) and `write_rtlil` land first. The evidence is per-row goldens in both latch modes, the Yosys 0.55 bridge, micro and VTR sweeps, fuzzers and the mcml.v/lenet.v benchmark. No `proc`, no `opt`, no `--x-report` (2H's).

**Architecture:** `src/frontends/verilog/elab1*.c` is stage 1: a run context (`odin3_elab1`) with a definition table, a specialization table keyed by resolved parameter values, and a FIFO worklist. Each specialization is walked twice. Pass E evaluates the module, records every declaration in an environment and every generate decision in a log. Pass W replays the log and emits the elaborated nodes bottom-up through the 2A builders. `src/frontends/elab/` holds what both front ends share:
- the 4-state constant values (`cval*.c`), the two-sweep EL-13 typer (`ctype.c`), and the evaluator and statement interpreter behind a host callback interface (`ceval*.c`);
- the binder, which fills the 2A symbol table and validates the EL-4 form (`bind.c`);
- stage 2's module emitter (`elab2*.c`): ports for every specialization before any body, then per module wires → mem2reg decision → initial values → instances → assignments → processes → wired-net gates → `$mem`, with a driver table checked before anything is attached;
- the process generator (`pg*.c`). It follows Yosys's `ProcessGenerator`, builds each body off-IR with `lv`/`rv` maps and undo logs, and makes one `$proc` node per block with `odin3_node_create_connected`.

`src/ir` gains the nine word cells and the `$proc` type: real `I`/`O` ports, an opaque `ODIN3_VAL_PROC` body blob in canonical layout checked by an allocation-free `verify`, and a per-module counter. It also gains the port fields of change C and one RTLIL sigspec spelling. The pass manager refuses `processes = REFUSE` passes on a design with processes. `src/backends/rtlil` writes RTLIL, processes included, so Yosys 0.55 can lower 2D's output for comparison.

**Tech Stack:** C17, `src/util`, `src/ir` (+ IR changes A/B/C), `src/ast` (store, builders, number values, symbol table, check, dump, print), 2C (`odin3_read_verilog_files`, the `read_verilog` pass, `tests/unit/verilog_test.h`), 2B-1 project record (`top`, `top_at`, `params`), the 1D pass manager, CLI and ABI, 1E simlib word helpers (`sim/word.h`), Unity, Python 3 stdlib (bridge, sweeps, generators), libFuzzer (clang, optional), Yosys 0.55 (`$VTR_ROOT/build/bin/yosys`; 0.69 only for the nightly drift job), ABC through `tools/equiv-check`, Icarus (simlib cases), `systemd-run --user` (memory caps).

**Spec:** `docs/specs/2026-10-11-2D-elaboration-design.md` **v4**. It is binding: EL-n and § numbers below refer to it, IR changes A/B/C (§7.2, §8.1, §6.2) are part of it, and its §9 message texts are verbatim requirements.
- v4, made on this branch with v2, amends EL-39 (ruling C1: the write temporaries start each block as `addr = x`, `data = x`, `en = 0`, as in Yosys 0.55 `simplify.cc` 3167–3231).
- v4 also amends EL-30's `~&`/`~|` row (minor M8: `$logic_not` of the reduction, as Yosys's parser builds them).

Also:
- `docs/PHASE2.md` #13: the rulings, decisions 1–13 and the x exit-bar ruling. #4: the exit bar. #6: D1–D5. #8: the 2A carry-overs, among them `odin3_symtab_remap` in `compact`.
- `docs/IR.md` (IR-1…IR-18, §9 rules) and the op registry `src/ir/cells/`.
- The 2A spec: §4.3 slot table, §4.4 flags, §4.5 numbers, §4.6 elaborated form, §7 attributes and §8 symbol table. Also `src/ast/*.h`.
- What 2C hands over: its spec §3.6, §4.2 and §7.3, its plan points (i), (l) and (m), and `tests/verilog/reject-2d/`/`ignore-2d/`.
- `docs/specs/2026-10-09-2C-verilog-coverage.md` (rows `L1`…`S4`: 100 MUST, 25 SHOULD, 10 IGNORE, 24 REJECT).
- 1E (`docs/specs/2026-10-09-1E-sim-design.md`: simlib semantics, the hook contract) and 1D (`docs/specs/2026-10-09-1D-abi-design.md`).
- The oracle is Yosys 0.55 as pinned by VTR (`frontends/ast/simplify.cc`, `genrtlil.cc`, `frontends/verilog/verilog_parser.y`, `backends/rtlil/rtlil_backend.cc`).

**Ordering and bases.** `main` (92abc75) has 2A (#33), 2B-1 (#34), 2B-2 (#36), the 2C spec and plan (#35, #37) and the 2D spec (#40). This branch is rebased onto it. Three branches are in flight:
- 2C-1 (`feat/2C-parser`): the lexer and grammar, Tasks 1–3 of 7 done.
- 2C-2 (`feat/2C-read`, not started): `read_verilog`, `--parse-only`, `tests/unit/test_read_verilog.c`, `run-reject`, and the stub `odin3_elab_run` in `src/frontends/verilog/elab.h` with `elab_stub.c`.
- 2B-3 (`feat/2B-passes`): the passes and CLI, Tasks 10–11 done; ABI v4 additive (ca6ff1c).

Seven PRs, as spec §18:

| PR | Branch | Base | Tasks | Waits for |
|---|---|---|---|---|
| 2D-1 IR | `feat/2D-ir` | `origin/main` now | 1–6 | **Nothing; Tasks 1–3 can start before 2C lands.** Details below the table. |
| 2D-2 constants | `feat/2D-const` | `origin/main` now | 7–9 | Nothing for Tasks 7–8 (hand-built trees). Task 9's interpreter tests parse with 2C-1's `vt_parse` and its function grammar, so rebase onto `main` after 2C-1 merges, before Task 9. |
| 2D-3 hierarchy | `feat/2D-hier` | `main` after 2C-2 and 2D-2 | 10–13 | 2C-2 merged (`odin3_read_verilog_files`, the pass, `elab.h`/`elab_stub.c`, `odin3_ast_library_units_from`, `run-reject`). Stage 1 never touches the IR, and the ABI stays v4 (point (c)), so 2D-1 is **not** required, nor is 2B-3. |
| 2D-4 stage-1 content | `feat/2D-content` | `main` after 2D-3 | 14–16 | 2D-3 |
| 2D-5 stage-2 module | `feat/2D-stage2` | `main` after 2D-1 and 2D-4 | 17–20 | 2D-1 (cells, ports, `write_rtlil`, the 0.55 CMake block) and 2D-4. 2B-3 only for the `--elab-only` + `--project` sugar (conflict note 9). |
| 2D-6 processes | `feat/2D-proc` | `main` after 2D-5 | 21–24 | 2D-5 |
| 2D-7 evidence | `feat/2D-evidence` | `origin/main` now; rebased after each 2D PR merges | 25–28 | It grows from 2D-3 and opens last, after 2D-6. Task 26's corpus run needs 2B-3's `project-fixtures compare`. Task 28's fuzz target needs the `ODIN3_FUZZ` block that 2B-3 or 2C-2 creates. |

Why 2D-1 waits for nothing:
- Tasks 1–3 (the IR changes) touch `src/ir`, `docs/IR.md`, `docs/DESIGN.md` §5.2/§6 and their tests.
- They also touch:
  - mechanical initializer fixes in test files (Task 2);
  - two public enum values in `odin3.h`, in hunks other than 2B-3's (Task 3);
  - the `ODIN3_VAL_PROC` arms of the JSON and Verilog writers and the Verilog writer's operator table (Tasks 1 and 3).
- No in-flight branch edits any of those hunks, so Tasks 1–3 start **before 2C lands**.
- Tasks 4–6 touch the pass manager, `builtin.c`, the writers' entries, `src/api` and `odin3.h`, which 2B-3 and 2C-2 also edit (conflict notes 4–8). Rebase onto whichever of them has merged before Task 4 starts; otherwise whichever lands second rebases.

**No file is cherry-picked between branches** (ruling C2). Every file lands in the PR of the task that creates it:
- Task 5 creates the Yosys-0.55 CMake block (point (p)).
- Task 21 creates the bridge: `tools/elab-check/elab-equiv`, `tools/elab-check/elab_equiv.py`, `tools/elab-check/memcap.py` (point (n)'s cap wrapper) and `tests/tools/test_elab_equiv.py`.
- Task 25 creates `elab-check` and the rows.

"Grows from 2D-3" works like this. The row sources and the hand-written stage-1 goldens (Task 25, no code) are written on `feat/2D-evidence` while 2D-3 is in review. The goldens stay provisional until 2D-4 merges (point (bb)). Before each of 2D-3…2D-6 opens, the controller merges that PR's head into a local scratch copy of the evidence branch (never pushed) and runs `elab-check` on it, with `--stage1` before 2D-5. The result goes in the PR description. The evidence CTests themselves land with 2D-7.

**Model/effort** (PHASE0 §9; PHASE2 row 2D is Opus `xhigh`, review by Peter on semantics; `high` once settled, as spec §18 says):

| Task | Implementer | Reviewer |
|---|---|---|
| 1 IR change A: nine word cells | Opus `high` (registry entries) | Opus `xhigh` |
| 2 IR change C: port range and sign | Opus `xhigh` | **Fable**, else Opus `xhigh` |
| 3 IR change B: the `$proc` cell | Opus `xhigh` | **Fable**, else Opus `xhigh` |
| 4 Passes and processes, writer refusals, `compact` symtab remap | Opus `high` | Opus `xhigh` |
| 5 `write_rtlil`, the Yosys-0.55 CMake block | Opus `high` | Opus `high` |
| 6 ABI additions (v4) | Opus `high` | Opus `high` |
| 7 Values, operators, the Yosys 0.55 constant table | Opus `xhigh` | Opus `xhigh` |
| 8 Expression typing | Opus `xhigh` | **Fable**, else Opus `xhigh` |
| 9 Evaluator, interpreter, system functions | Opus `xhigh` | Opus `xhigh` |
| 10 Stage-1 context, entry, lookup, top, pending kinds, `run-reject` | Opus `xhigh` | Opus `xhigh` |
| 11 Parameters, specializations, passes E/W, push-down | Opus `xhigh` | **Fable**, else Opus `xhigh` |
| 12 Instances, connections, arrays, black boxes | Opus `xhigh` | Opus `xhigh` |
| 13 Binder and EL-4 validation | Opus `high` | Opus `xhigh` |
| 14 Generate and scope names | Opus `high` | Opus `xhigh` |
| 15 Declarations, ports, implicit nets, normalisations | Opus `high` | Opus `high` |
| 16 REJECT/IGNORE diagnostics, cap, handover expectations | Opus `high` | Opus `high` |
| 17 Stage-2 context, wires, ports, provenance, keep levels, budget, `--elab-only`, `odin3_ast_elaborate` | Opus `xhigh` | Opus `xhigh` |
| 18 Expressions and selects to word cells | Opus `xhigh` | **Fable**, else Opus `xhigh` |
| 19 Instances, gates, assignments, drivers, wired nets, tri-state | Opus `xhigh` | Opus `xhigh` |
| 20 Memories (decision, `$mem`, reads) | Opus `xhigh` | Opus `xhigh` |
| 21 Process generator core, the bridge | Opus `xhigh` | **Fable**, else Opus `xhigh` |
| 22 `if`/`case`, case temporaries, `full_case`/`parallel_case`, lvalue selects | Opus `xhigh` | Opus `xhigh` |
| 23 Loops, functions and tasks, continuous-context calls | Opus `xhigh` | Opus `xhigh` |
| 24 Memory writes, initial values, `$readmem`, T9/T10 | Opus `xhigh` | Opus `xhigh` |
| 25 `elab-check`, rows, goldens, the sweep preset | Opus `high` (bulk generated goldens: Sonnet `high`) | Opus `high` (re-derives every hand golden) |
| 26 Project corpus, handover close-out, micro sweeps | Opus `high` | Opus `high` |
| 27 VTR sweep, `bench_elab`, PHASE2 entry | Opus `high` (bench runs: Sonnet `high`) | Opus `high` |
| 28 Fuzzer and differentials | Opus `high` | Opus `high` |
| Whole-branch review before **each** PR (2D-1 … 2D-7) | — | **Fable**, else Opus `xhigh` |

Effort cues:
- Say `/model opus` + `/effort xhigh` at the start of 2D-1 Task 2, and keep `xhigh` through 2D-3 and through 2D-5/2D-6.
- Say `/effort high` for Tasks 1, 4–6, 13–16 and 25–28.
- Say `/model sonnet` + `/effort high` for bulk golden generation and the bench runs.
- Say `/clear` between tasks (one task per session).

**Lanes** (land one at a time on each branch; review I2):
- **2D-1:** 2 → 1 → 3 → 4 → {5 ‖ 6}.
  - Tasks 1 and 2 share `cells.h` and `test_sim_word.c`.
  - Task 3's cell uses Task 2's port macros.
  - Task 5 needs Task 4's `processes` member and shares `write_formats.c`, the pass table and the docs with it.
  - Tasks 5 and 6 share only `CMakeLists.txt` (appends).
- **2D-2:** 7 → 8 → 9, in parallel with all of 2D-1.
- **2D-3:** 10 → 11 → 12, with 13 alongside 11–12 (`bind.c` and hand-built stores share no file with stage 1).
- **2D-4:** 14 → 15 → 16. They share pass E/W.
- **2D-5:** 17 → 18 → 19 → 20.
- **2D-6:** 21 → 22 → 23 → 24. They share `odin3_pg`.
- **2D-7:** Task 25's sources and provisional hand goldens start with 2D-3. Then 25 → 26 → 27 → 28 after 2D-6.

Stage 1 (2D-3/2D-4) and the IR (2D-1) are independent lanes until 2D-5.

**Pre-flight conflict note.**
1. `CMakeLists.txt`: every task appends sources to `odin3_core`, `odin3_add_unit_test(...)` lines and CTests. These are append-only hunks; keep both sides.
   - Task 5 adds the `ODIN3_YOSYS055` discovery block (point (p)). Every later Yosys-0.55 CTest goes inside it.
   - Task 25 adds the `sweep` label convention, and `CMakePresets.json` gets the `debug`/`release` exclusion and the `sweep` test preset (point (n)).
2. `src/ir/cells/builtin.c` and `cells.h`: Task 1 appends the nine cells and Task 3 appends `$proc` (point (f)). Append-only.
3. `src/ir/celltype.h`, `celltype.c` and `src/ir/module.h`:
   - Task 2 appends `is_signed` to `odin3_port_def` and four fields to `odin3_port_spec`. Every positional initializer of either struct gains the new trailing values: the cells macros, `src/ir/cells/const.c` and `tests/unit/*.c` including `test_techlib_reader.c` and `test_blif_reader.c`. `-Wmissing-field-initializers` under `-Werror` finds them all.
   - Task 3 raises `celltype.c`'s parameter-kind bound to `PROC` and adds the `$proc`-only rule (point (g)).
4. `include/odin3/odin3.h` stays at **ABI v4** (point (c); no version change anywhere):
   - Task 3 adds `ODIN3_VAL_PROC` (4) and `ODIN3_CT_PROCESS` (32).
   - Task 4 adds the public `ODIN3_PASS_PROCESSES_*` constants and renames `odin3_plugin_pass.reserved[0]` to `processes`.
   - Task 6 adds `odin3_module_get_proc_count` and writes the v4 history line "(cont., 2D)".
   - Task 10 adds `odin3_ast_get_library_units_from`.
   - Task 17 adds `odin3_elab_options`, `odin3_ast_elaborate` and the failed-read note.
   - Tasks 10 and 17 extend the same v4 line.
   - 2B-3 adds `odin3_design_has_project`, `odin3_pass_set_revision` and `odin3_pass_set_diag_paths` under v4 in other hunks.
   All hunks are additive. Whichever lands second extends the line rather than duplicating it.
5. `src/passes/manager.{h,c}`: Task 4 appends `uint8_t processes` to `odin3_pass_def` and the refusal in `run_def`. 2B-3 extends `odin3_pass_options` and `run_def` (diag paths). Different hunks, but both in `run_def`: the refusal goes **before** `odin3_pass_run_begin`, and 2B-3's diag-path line stays first.
6. `src/passes/builtin.c`, `builtin.h`, `write_formats.c`, `sim_pass.c`:
   - Task 4 adds a `processes` value to every positional `odin3_pass_def` initializer and the remap in `compact`.
   - Task 5 then adds `write_rtlil`.
   - 2B-3 adds `read_project`, `dump_project` and `preprocess` (`project_pass.c`); 2C-2 adds `read_verilog`. Whichever lands second gives the other's new defs their `processes` value (point (x)).
7. `src/api/{pass.c,node.c,module.c,ast_abi.c}` and the new `src/api/elab_abi.c`: Tasks 4, 6, 10 and 17. 2B-3 edits `src/api/pass.c` and `design.c` (setters): different functions. `src/api/celltype.c` is unchanged: plugins stay bounded at `COVER`. `plugins/python/odin3.py` reads the cdef verbatim; Task 6 only adds the kind name.
8. Backends:
   - `src/backends/json/emit.c` and `src/backends/verilog/writer.c`: Task 3 adds the `ODIN3_VAL_PROC` switch arms. These two files and `src/ir/value.c` are the only `-Wswitch` sites.
   - `src/backends/verilog/writer.c`: Task 1 adds seven `BUILTINS` rows and the `$shiftx`/`$pow` refusal.
   - `src/backends/{blif,json,verilog,dot}/writer.c`: Task 4 adds the refusal at each writer's entry.
   Nothing else in flight touches these files.
9. `src/cli/main.c` and `tests/cli/test_cli.sh`: Task 17 adds `--elab-only` and `cli_elab_only*`. 2C-2 adds `--parse-only`; 2B-3 adds `--project`/`-f`/`--revision`/`--diag-paths`. `--elab-only` mirrors 2C's `--parse-only` code path. Whichever of 2D-5 and 2B-3 lands second adds the `--project`/`-f` + `--elab-only` append rule, as 2C conflict note (5) does for `--parse-only`.
10. 2C-2's files: `src/frontends/verilog/elab.h`, `elab_stub.c`, `read_verilog.c`, `tests/unit/test_read_verilog.c`, `tools/parse-check/run-reject` and `tests/tools/test_run_reject.py`. **All are Task 10's** (review I4):
    - Task 10 changes the `odin3_elab_run` signature and store ownership (point (a)), deletes `elab_stub.c`, and adds the three flags to `read_verilog`'s option parse, usage and call.
    - Task 10 flips 2C's stub expectation in `test_read_verilog.c` and makes `run-reject --elaborate` judge per case (point (l)).
    - Task 16 only flips `reject-2d`/`ignore-2d` expectations. It may correct a `.err` or `.warn` text only to match a §9 table row, quoting that row in the commit message.
11. `src/ast/*`: 2D adds no AST API. It consumes 2C-2's `odin3_ast_library_units_from`. `docs/specs/2026-10-09-2A-ast-design.md` gets §19's two amendments: Task 15 the §4.6 pointer to EL-4, Task 22 the §7 D1 sentence. 2C-2 makes its §9 (b) there. These are different paragraphs.
12. Docs:
    - `docs/IR.md`: Tasks 2 and 3, with IR-12's note made **once**, in Task 3.
    - `docs/DESIGN.md`: Tasks 1, 3, 5 and 10.
    - `docs/PASSES.md`: Tasks 4, 5, 10 and 17; 2B-3 and 2C-2 add rows. Rows are independent lines; keep both.
    - `docs/specs/2026-10-09-2C-verilog-coverage.md` E12: Task 8.
    - `docs/specs/2026-10-09-1D-abi-design.md`: Task 4.
    - `docs/specs/2026-10-11-2D-elaboration-design.md` v4: this branch only.
    - `docs/PHASE2.md` is append-only: Task 27 adds 2D's entry as the next free number (#14 unless 2C took it).
13. `tests/unit/simlib_word_cases.h` and `tools/sim-check/gen_simlib_cases.py`: Task 1 only. Regenerate, never hand-edit.
14. 2C-2's `tools/verilog-gen/*` and the fuzz CMake block: Task 28 extends them (`--elab`, `fuzz_elab`). `tools/project-fixtures/project_fixtures.py` (2B-3): Task 26 adds the `--with-frontend` bridge mode. `tools/equiv-check` is not touched by 2D (`--x-report` is 2H's, point (m)).
15. Golden directories `tests/elab/rows/`: Task 25 writes them. A later task that finds a hand golden wrong fixes it only with the EL rule or Yosys 0.55 run that proves it quoted in the commit message, and the reviewer re-derives that file.

**Spec points made exact here** (agent defaults, reversible; Task 27 logs them in PHASE2):

- (a) **Options reach the entry; the parsed store's owner is explicit.** 2C's stub signature has no options channel. The entry becomes `odin3_status odin3_elab_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_ast *parsed, const odin3_elab_opts *opts)`.
  - `read_verilog` parses `--nolatches`, `--nomem2reg` and `--auto-blackbox` into `opts` and leaves `top`/`top_at` 0. Step (1) fills them from `--top` (`odin3_pass_get_options().top`, interned, `top_at` 0), else from the project record's `top`/`top_at`.
  - Ownership (review I10):
    - `read_verilog` hands the parsed store to the design (`odin3_design_set_ast`) **before** the call.
    - Step (3) is `odin3_design_drop_ast(design, run, PARSED)`, which destroys it unless the keep level is `ALL`, so the parsed store is gone before stage 2 as §15's estimate assumes.
    - The pass's own later drop is a no-op ("a store not held is a no-op").
    - `odin3_elab_run` never calls `odin3_ast_destroy` on `parsed`. `elab.h` documents this.
- (b) **ABI entry.** `odin3_pass_ctx` is not in the ABI, so the ABI form is `odin3_status odin3_ast_elaborate(odin3_design *design, odin3_ast *ast, const odin3_elab_options *opts)`.
  - The store must be held by `design` (made with `odin3_design_new_ast`), sealed and of form `ELABORATED`; anything else is `INVALID_ARG`.
  - The provenance run is the store's (`odin3_ast_get_run`), from which the call builds the internal context (`op` 0).
  - It runs EL-2 steps 4–5. Step 6 is moot because the design already holds the store. `opts->top` and `auto_blackbox` are stage-1 options, ignored here.
  - Its paragraph says that a plugin cannot yet learn a pass-run ID (PHASE2 #8 carry-over). Until that ABI addition, the slang adapter supplies the run of the read it performs itself.
- (c) **ABI stays at the unreleased v4** (ruling I1). Every 2D addition is additive in 1D's sense:
  - new functions;
  - enum constants appended at the end;
  - a reserved slot whose old required value, NULL, keeps the safe old meaning (`REFUSE`).

  `ODIN3_ABI_VERSION` stays 4. The header's v4 history line gains "(cont., 2D): $proc processes and process counts, value kind PROC, pass processes slot, library units, odin3_ast_elaborate", written by Task 6 and extended by Tasks 10 and 17. The bump is decided once, at the first release or Phase-2 close, as a PHASE2 decision (Task 27 records the item).
- (d) **IR change C has no ABI mirror.** No ABI function creates modules or ports, and `odin3_plugin_port` stays frozen (§6.2).
- (e) **`ODIN3_VAL_PROC` payload** is one blob in the value's `bits`/`len` (`len` = bytes).
  - Layout: a header of six `uint32_t` counts (pbits, compares, actions, cases, switches, syncs), then the six arrays in that order as the §8.1 C structs, each array at an 8-byte-aligned offset.
  - `odin3_proc_body_pack` writes every record field by field into zeroed arena memory (`max_align_t`-aligned), so padding bytes are 0. Hence `odin3_value_copy`, `odin3_value_equal` (byte compare), `compact` and tombstones need nothing beyond `has_payload` accepting `PROC`.
  - `odin3_proc_body_view` rejects a `bits` pointer that is not 8-byte aligned (no misaligned casts under UBSan).
  - **Canonical layout** (review I3), which `verify` checks without allocating:
    - case 0 is the root;
    - the switches of case c are a contiguous range, the ranges in case-index order partitioning `[0, n_switches)`;
    - the cases of switch s are a contiguous range, the ranges in switch-index order partitioning `[1, n_cases)`;
    - every case of switch s has a higher index than the case owning s;
    - the actions of case c are contiguous, the ranges in case order partitioning `[0, A)`;
    - the sync actions follow, contiguous per sync in sync order, ending at `n_actions`;
    - compares are contiguous per case, in case order.

    The process generator emits this order (Task 21: breadth-first numbering). "Every case, switch and action used exactly once" is then O(body) running-counter checks.
- (f) **Table order.** The nine cells (`$neg $xnor $reduce_xnor $reduce_bool $logic_not $logic_and $logic_or $shiftx $pow`), then `$proc`, are appended after `$sop` (index 52). Built-in IDs 0–52 do not move; the count becomes 63.
- (g) **`$proc` type.**
  - Ports are `I` and `O` (`ODIN3_PORT_VEC` on `I_WIDTH`/`O_WIDTH`).
  - Parameter defaults are `I_WIDTH` 0, `O_WIDTH` 1 and an empty `BODY`, which `verify` rejects, so a default node cannot exist.
  - `ODIN3_CT_PROCESS` = 32.
  - `celltype.c`'s kind bound rises from `COVER` to `PROC` (review I14). Registration rejects a `PROC` parameter or the `ODIN3_CT_PROCESS` flag on any definition other than `&odin3_cell_proc`, with `cell type '<t>': only the built-in $proc may have a PROC parameter or ODIN3_CT_PROCESS`.
  - `src/api/celltype.c` keeps its `COVER` bound, so a plugin can declare neither.
- (h) **A sync on an `OUT` position is legal** for edge and level syncs. §8.1 is normative and carries re-review N1. §14.1's "a sync signal that is an OUT position" among the rejected bodies is a stale v2 phrase. Task 3 asserts acceptance. Task 27 logs it as a spec erratum.
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
  - `elab_test.h`: limits and hooks.
  - `src/frontends/verilog/elab1.h`: the stage-1 context.
  - `src/frontends/verilog/elab1_inst.c`: instances and connections, added to §12's list.
- (k) **The run-level iteration cap crosses the seam without a field.** Stage 2 starts its `ODIN3_ELAB_MAX_TOTAL_ITERATIONS` count at the number of indexed `GEN_BLOCK`s in the elaborated store, one per generate-loop iteration stage 1 kept. The error count needs no carry, since stage 2 never starts after a stage-1 error. Task 23 tests the sum.
- (l) **Not-yet-built constructs: a located error, so `main` stays green after every PR** (ruling C3).
  - `src/frontends/elab/elab_pending.c` holds one static table per stage, `k_pending[stage][]`, of the AST kinds, plus three features (`ODIN3_PEND_COMB_CALL`, `ODIN3_PEND_REGS_VAR_READ`, `ODIN3_PEND_MEM_WRITE`), whose elaboration has not landed yet.
  - Each task that implements a kind removes it from the table in the same commit. The staging is:
    - Task 11 lands `MODULE`, `LIST`, ANSI `PORT_DECL`, `PARAM_DECL`, `NET_DECL`/`VAR_DECL` without strength or delay, `RANGE`, `CONT_ASSIGN`/`NET_ASSIGN`, `INSTANTIATION`/`INSTANCE`/`CONNECTION` (named connections), `DEFPARAM` and expressions as written.
    - Task 12 lands ordered connections, arrays and `GATE_DECL`.
    - Task 14 lands `GENERATE`, `GEN_FOR`, `GEN_IF`, `GEN_CASE`, `GEN_BLOCK`, `GENVAR_DECL` and scope `HIER_NAME`s.
    - Task 15 lands `PORT_REF` merging, implicit nets, `FUNCTION_DECL`, `TASK_DECL`, `ALWAYS`, `INITIAL` and every statement kind.
    - Task 16 lands every REJECT/IGNORE kind; the stage-1 table is then empty.
    - In stage 2:
      - Task 17 lands declarations, `PARAM_DECL`, ports and the scope kinds (`MODULE`, `GEN_BLOCK`, named `SEQ_BLOCK`, `FUNCTION_DECL`/`TASK_DECL` as scopes);
      - Task 18 lands `CONT_ASSIGN`/`NET_ASSIGN` (net declaration assignments included);
      - Task 19 lands `INSTANTIATION`/`INSTANCE`/`CONNECTION`/`GATE_DECL`, the driver table and A4;
      - Task 21 lands `ALWAYS` with blocks and assignments;
      - Task 22 lands `IF` and `CASE`;
      - Task 23 lands `FOR`, `REPEAT`, `TASK_CALL`, calls in processes and the two read features;
      - Task 24 lands `INITIAL` (variable initialisers included) and `ODIN3_PEND_MEM_WRITE`.
  - Reaching a pending kind or feature logs, once per kind per run, `file:line:col: error: elaboration is not available yet (2D): '<KIND>'`. `<KIND>` is the dump's kind name, or `continuous-context call` / `variable index into a register array` / `memory write` / `variable index in a procedural assignment target` (Task 21; Task 22 lands it). It counts as an error.
  - A stage-1 error destroys the elaborated store, so rule 7 never sees an unhandled kind. A stage-2 error skips the construct, so the IR stays valid.
  - Until Task 17, `odin3_elab_stage2` is the stub `src/frontends/elab/elab2_stub.c`, which logs 2C's exact unlocated text `elaboration is not available yet (2D)` and returns `PARSE`.
  - Task 10 makes `run-reject --elaborate` judge per case. An expected line found → **pass**. Else a line containing `elaboration is not available yet (2D)` → **pending** (listed, not failed). Else → **fail**. Exit 1 on any fail, 77 when every case is pending.
  - Hook `odin3_elab_test_pend(kind)` (elab_test.h) forces a kind pending, so the mechanism stays testable after the tables are empty.
  - Task 16 asserts the stage-1 table is empty. Task 24 asserts both are, plus CTest `elab_no_pending`: the coverage rows and the sink never print the text.
- (m) **`--x-report` and the defined-output miter stay in 2H** (ruling I13; spec §18, PHASE2 #13). 2D-7 produces only what 2H needs:
  - `elab-equiv --a-only --keep DIR` (Task 21) writes side A's netlist as `a.blif` with Parmys's `write_blif -gates -undef + unconn` convention, for 2H to compare with Parmys goldens;
  - Task 27 writes the 2H backlog line in 2D's PHASE2 entry.

  Of §14.6's items, 2D owns item 1 (x kept explicit, asserted by Task 18's `$shiftx` and wildcard `===`, and Tasks 21–22's x constant pins). Item 2 is 2F's; items 3–5 are 2H's.
- (n) **Memory and sweeps** (WSL rule; ruling I12).
  - *Amended (2E/2F plans v2; controller ruling C1).* Every sweep runs through `tools/heavy-run` (2E plan point (p); created by 2E-1 Task 2, or identically by 2D-7 if it lands first). `heavy-run` takes `/tmp/claude-1000/odin3-heavy.lock` exclusively on file descriptor 9, which the job inherits (the lock lives as long as the job's processes), and starts the job in the named slice `odin3-heavy.slice` (MemoryMax=6G, MemorySwapMax=0), exporting `ODIN3_HEAVY_SLICE`. Inside a heavy run (`ODIN3_HEAVY_SLICE` set) a nested `heavy-run` runs its job in place, without the lock (*amended by 2E Task 2's review*).
  - Every child a sweep driver starts (odin3, Yosys, equiv-check, `bench_elab`) runs through `tools/elab-check/memcap.py` under `systemd-run --user --scope -q --slice=$ODIN3_HEAVY_SLICE -p MemoryMax=<cap> -p MemorySwapMax=0`, so the slice bounds the job and all its children together. Never a bare nested `--scope`: it lands in `app.slice`, outside the cap. Without `ODIN3_HEAVY_SLICE` (an ad hoc run), a child gets its own scope as before. This holds for ASan binaries too.
  - When `systemd-run --user` is unavailable: a Release child runs under `prlimit --as=<cap>`, and an ASan child makes the driver exit 77 (skip).
  - `--jobs J` is refused when `J × cap` exceeds 60% of `MemTotal`, or exceeds the slice's MemoryMax inside a slice. Defaults: J = 1, cap 4 GiB; `lenet.v`'s Odin III child gets 5 GiB, alone, in the 6G slice.
  - *Amended (ruling C1).* A run needing more than 6 GB carries the CTest label `sweep-xl` instead of `sweep`. That is every Yosys-side run on `lenet.v` (≈ 7 GB), so `elab_equiv_vtr`'s lenet leg becomes `elab_equiv_vtr_xl`. It runs through `tools/heavy-run --xl` in `odin3-heavyxl.slice` (MemoryMax=9G, MemorySwapMax=0), with an 8 GiB child cap. (Not `odin3-heavy-xl.slice`: systemd nests a slice under the one its name extends at a dash, so that name would keep the 6G cap; 2E Task 2.) The controller starts it by hand, only when no agent is running (Peter's rule), by exporting `ODIN3_HEAVY_XL_OK=1`.
  - *Amended (ruling C1; re-review).* Every build runs through `tools/build-locked <cmake --build args>` (2E-1 Task 2):
    - it waits while a `/tmp/claude-1000/odin3-heavy.queued.<pid>` file names a live `heavy-run` (a heavy job queued for the lock: writer preference; a stale flag is removed);
    - then it takes `/tmp/claude-1000/odin3-heavy.lock` shared;
    - it refuses (exit 2) inside a heavy run (`ODIN3_HEAVY_SLICE` set), where its shared lock would wait forever.

    So no heavy run overlaps a build in any worktree, and builds cannot starve a heavy run. The `ctest -j4` gate runs outside the lock by design: its tests are light.
  - A child killed by its cap is reported `out of memory (cap N)`, never retried in parallel.
  - Heavy sweeps carry the CTest label `sweep` and `RUN_SERIAL TRUE`:
    - `elab_coverage_equiv`, `verilog_micro_elab`, `elab_equiv_micro`, `verilog_vtr_elab`, `elab_equiv_vtr`, `elab_gen_vs_yosys`, `elab_const_diff` and `bench_elab_smoke`;
    - any ASan run touching `lenet.v`.
  - `CMakePresets.json`'s `debug` and `release` test presets exclude `sweep` and `sweep-xl` (`"filter": {"exclude": {"label": "sweep|sweep-xl"}}`), so the `-j4` gate never runs them; a heavy CTest reached anyway skips (exit 77) without `ODIN3_HEAVY_OK=1`. A new `sweep` test preset (configure `release`, `"filter": {"include": {"label": "^sweep$"}}`, `"execution": {"jobs": 1}`, `"environment": {"ODIN3_HEAVY_OK": "1"}`) runs them. A `sweep-xl` test preset (the same with `"include": {"label": "sweep-xl"}}`) runs the xl tests; `ODIN3_HEAVY_XL_OK` is never in a preset. Every heavy CTest is registered through `odin3_add_sweep_test()` (2E-1 Task 2), which gives it a `TIMEOUT` of 43200 s that covers `heavy-run`'s lock wait (at most 14400 s, then exit 77) and the run. There is no workflow preset for sweeps. The controller builds Release with `tools/build-locked --preset release -j6`, then runs `ODIN3_HEAVY_OK=1 ctest --preset sweep -R '<prefix>'`. A build is never started under `heavy-run`: its shared lock would deadlock against the exclusive one.
  - The 64 VTR files (31 `verilog/*.v` + 33 `koios/*.v`) are one set, sorted by size together: `lenet.v` is last and `mcml.v` falls where its size puts it. Every runner says smallest first.
- (o) **Micro `--top`** (EL-7, ruling 6) is the first `.model` of `~/odin3-ws/golden/k6_frac_N10_frac_chain_mem32K_40nm/regression/verilog/<group>/<leaf>/<leaf>.parmys.blif`, read by `tools/elab-check/micro-top`.
- (p) **Yosys 0.55 discovery** (Task 5 creates the block). `find_program(ODIN3_YOSYS055 yosys HINTS $ENV{VTR_ROOT}/build/bin $ENV{HOME}/odin3-ws/external/vtr-verilog-to-routing/build/bin NO_DEFAULT_PATH)`, and `yosys -V` must contain `Yosys 0.55`. Without it no 0.55 CTest is registered: CI has none, so they run locally and nightly. 0.69 is the existing `ODIN3_YOSYS`, used only by the nightly drift job. Every generated table and golden records `yosys -V` in its header.
- (q) **Name collisions** get `$2`, `$3`, … in specialization creation order (FIFO), so names are deterministic. A name longer than 128 bytes is hashed **before** the collision check.
- (r) **RTLIL process names.** A `$proc` node prints as `$proc$<file>:<line>$<ID>`, with file and line from the node's SOURCE record (the file's given path), else `$proc$<ID>`.
- (s) **One sigspec spelling.** `odin3_proc_sigspec` (`src/ir/proc.h`, Task 3) spells a span LSB-first-in, MSB-first-out:
  - constant-net and `ANY` runs merge into `<n>'<bits>` (`-` for `ANY`);
  - consecutive bits of one wire's primary merge into `\w [hi:lo]`, a whole wire is `\w`, and a single bit is `\w [k]` (`\w` for a 1-bit wire);
  - another net is `\name`, else `$n<ID>`;
  - several chunks go in `{ … }`.

  The body printer, `write_rtlil` and `odin3_node_get_param_text(BODY)` all use it, so one fixture text serves every test (review M5).
- (t) **`bench_elab`** is a new `tests/bench/bench_elab.c` (§15's name). `--set` runs are `elab-sweep --bench`, which starts `bench_elab --one F` per file.
- (u) **Test files.** Stage 1: `test_elab_stage1_lookup.c`, `_params.c`, `_inst.c`, `_gen.c`, `_decl.c` and `_diag.c`. Stage 2: `test_elab_stage2_wires.c`, `_expr.c`, `_drive.c`, `_mem.c`, `_proc.c`, `_case.c`, `_call.c` and `_init.c`.
- (v) **Warn once.** A construct elaborated several times warns once (§9.2). The key is `(parsed node ID << 32 | message-format index)` in a `u64map` on the stage-1 context. Stage 2 keys on the elaborated node's `loc`.
- (w) **Texts §9 leaves open:**
  - `body of black box 'm' is ignored` (info)
  - `module 'foo' is not defined; it becomes an implicit black box (--auto-blackbox)` (info)
  - `implicit black box 'foo': port 'a' is 4 bits at its first instance; this connection is 8 bits` (error)
  - `value of parameter 'P' of cell type 't' is not an integer` (error)
  - `--elab-only cannot be combined with a script; use read_verilog in the script` (CLI)
  - ~~`module 'm': cell '<name>' of type '$shiftx' has no structural Verilog form` (`write_verilog`, also `$pow`)~~ — *Amended (Task 1 fix round, controller ruling):* `write_verilog` writes `$shiftx` and `$pow` as generic instances with every parameter, as `$memrd`; there is no refusal and no text.
- (x) **`processes` values for other branches' passes.** `read_project`, `dump_project`, `preprocess` and `read_verilog` are `OK` (they never read the IR as logic). Every other existing pass is as §8.1 lists.
- (y) **`$readmem` paths.** A relative path resolves against the directory of the resolved path of the `FILE` buffer holding the call's `loc`. An absolute path is used as given (DESIGN §4.0).
- (z) **Text and count fixes.**
  - §4.7's "$clog2 needs a constant argument" and §9.1's `<what>` form disagree. Use §9.1's form: `$clog2 argument is not constant (it reads 'x')`.
  - §14.2's hand-written list names **54** rows, not "≈ 40". All 54 are hand-written.
  - §14.1's `test_ir_module.c` does not exist yet: Task 2 creates it.
  - The constant table reads values back from `write_rtlil` `connect` lines of module outputs, because `localparam`s do not appear in RTLIL. Each case is `assign y<k> = <expr>;`.
- (aa) **Cap texts** (review I6). §9 gives none for these. Each is located at the node named, and lowered hooks print the hook value:

  | Cap | Text | Enforced and tested in |
  |---|---|---|
  | specializations | `elaboration makes more than 65536 specializations` (at the instance creating the next) | Task 11 |
  | elaborated nodes | `elaboration needs more than 33554432 elaborated nodes` (at the node being written) | Task 11 |
  | instances | `elaboration makes more than 16777216 instances` (at the instance or gate) | Task 12 |
  | net bits | `elaboration needs more than 67108864 net bits` (at the declarator or the expression) | Task 17 |
  | cell pins | `cell of N pins exceeds the limit of 4194304 pins` (at the expression) | Task 18 |
  | inline depth | `function call nesting deeper than 1024 at 'f'` (at the call) | Task 23 |
  | init bits | `initial values need more than 67108864 bits` (at the `initial` statement) | Task 24 |
  | readmem bytes | `'f.hex' is larger than 268435456 bytes (the $readmem limit)` (at the call) | Task 24 |

  The rest use §9.1's texts. Width is Task 8; loop, total and eval steps are Tasks 9, 14 and 23; call depth is Task 9; hierarchy depth is Task 11; memory bits is Task 20; bytes is Task 17.
- (bb) **Spelling of `NUMBER`s that stage 1 creates** (review I8), so the hand `.elab` goldens are exact:
  - **32-bit integer form:** a known value that fits a 32-bit signed integer, for genvar reads, folded `$clog2`, `REPEAT` and `REPLICATE` counts, `RANGE` and `SELECT` bounds and widths, and `GEN_BLOCK` indices. Sub `DEC`, flag `SIGNED`, payload width 32.
  - **Unsized parameter values:** a final `PARAM_DECL` value that is still unsized (EL-16) keeps the source literal's sub (base), with payload width 0 and `SIGNED` per value.
  - **Every other value:** sub `BIN`, payload width = the value's width, `SIGNED` per value. This covers sized parameter values, values with x/z, and attribute values.

  Hand goldens drafted while 2D-3 is in review are **provisional**. They are re-derived once against Task 15's tests after 2D-4 merges and frozen then (Task 25).

## Global Constraints

- **Earlier constraints.**
  - C17 with the `-Werror` flag set.
  - Functions ≤ 60 lines, CCN ≤ 15, ≤ 5 parameters, no adjacent convertible parameters (use request structs).
  - **No recursion in `src/`**: the typer, evaluator, interpreter, generate expansion, hierarchy walk, binder, emitter and process generator all use explicit stacks or worklists. Tests may recurse in reference models.
    - **Known deviation (PR 2D-6 review I6, recorded for 2D-7).** A call in an expression is inlined by C recursion (`pg_run` → `exec_node` → `assign_value` → `odin3_elab2_expr` → `pg_call_hook` → `odin3_pg_call` → `call_inline` → `pg_run`). Interim guard: `ODIN3_ELAB_MAX_INLINE_DEPTH` (256) gives a located error, so the stack cannot overflow (spec §17 #74). 2D-7: run the call after the expression walk (the walker hands back a placeholder) and drop the cap.
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
  - A failed stage 2 leaves valid, incomplete modules, and the top is not set. `odin3_check_design` FULL passes after every failing test.
- **Oracle.** Every "verified" assertion, constant table, golden and bridge run uses Yosys 0.55 (point (p)). 0.69 appears only in the nightly drift job, and the two are never mixed in one comparison (ORACLES.md).
- **Diagnostics.** Every located error and warning uses the exact §9 or point (aa)/(w) text, through `odin3_diag`: `file:line:col: error|warning|info: …`, lower case, names in `'…'`, no final period. DESIGN §4.0 kinds are written `<kind>: <detail>`. Every located-diagnostic test asserts the whole line through a capturing log sink. Errors are capped at 20 per run (`too many errors; elaboration stopped after 20`).
- **Caps, exact (§11)**, each a named constant in `src/frontends/elab/elab.h`, checked before the allocation it guards, reported as a located `PARSE` with its §9.1 or point (aa) text, and lowered in tests through `odin3_elab_test_set_limits` (`elab_test.h`):

  | Cap | Value |
  |---|---|
  | `ODIN3_ELAB_MAX_ERRORS` | 20 |
  | `ODIN3_ELAB_MAX_WIDTH` | 2^20 |
  | `ODIN3_ELAB_MAX_CELL_PINS` | 2^22 |
  | `ODIN3_ELAB_MAX_LOOP_ITERATIONS` | 2^20 |
  | `ODIN3_ELAB_MAX_TOTAL_ITERATIONS` | 2^24 |
  | `ODIN3_ELAB_MAX_EVAL_STEPS` | 2^26 |
  | `ODIN3_ELAB_MAX_CALL_DEPTH` | 1024 |
  | `ODIN3_ELAB_MAX_INLINE_DEPTH` | 256 |
  | `ODIN3_ELAB_MAX_LOOP_OBJECTS` | 2^21 |
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
  - Gates: `tools/build-locked --preset debug -j6 && ctest --preset debug -j4` green. The preset excludes labels `sweep` and `sweep-xl` (point (n)). Sweeps run with `ODIN3_HEAVY_OK=1 ctest --preset sweep -R '<prefix>'` after `tools/build-locked --preset release -j6`.
  - `/lint` before each commit; commit (and the controller pushes) at each green checkpoint.
  - `main` is green after every PR (point (l)).
  - A read-only critique per task; a **Fable** whole-branch review before each PR.
- **Every public function** in `odin3.h` gets the purpose/ownership/failure paragraph (CLAUDE.md).
- **Python** (`tools/`, `tests/tools/`): stdlib only, `ruff check`, `mypy --strict`, `unittest`.
- **Sweeps** run smallest input first, serially, under point (n)'s caps. No sweep runs while a build runs.

## Review Focus

1. **Parameterized instances as users write them.** For `module m #(parameter W = 1)`, users expect what Yosys 0.55 gives:
   - `#(.W(1))`, equal to the default, shares the base module, named `m`, not a `$paramod`;
   - `defparam u1.W = 7` loses to `#(.W(5))`, with the warning;
   - of two `defparam`s, the first wins;
   - a project `param top.u1.W 3` beats all three.

   Pinned in Task 11 (`test_elab_stage1_params`: the five-level matrix, names `m`, `m$W=5`, `m$W=8`, `m$W=6`, `m$W=3`).
2. **Mixed-width arithmetic and unsized constants.** With 8-bit `a`, `b`, `y`, a user expects:
   - `y = (a + b) >> 1` drops the carry (`$add` `Y_WIDTH` 8, as Yosys and IEEE);
   - `y = (0 + a + b) >> 1` keeps it (32-bit adds);
   - `y8 = &a` gives a `$reduce_and` of `Y_WIDTH` 8, as Yosys;
   - `localparam P = 'hFFFFFFFF`, and `[31:0] Q` of the same, assigned to a 64-bit output give all ones, while `[35:0] R` gives zero-extension.

   Pinned in Task 8 (the typer's widths) and Task 18 (the cells' `Y_WIDTH`s and the P/Q/R outputs).
3. **Clocked blocks that mix `=` and `<=`.** In `t = a & b; r <= t;`, `r` must take the new `t`, and `t` is a second output whose register `opt` later removes. `q <= q + 1` reads the held value. Pinned in Task 21 (the exact `$proc` bodies of T7 and T8, the `OUT`-read semantics, and their bridge CTest).
4. **Memories with non-zero bases and init files.** For `reg [7:0] m [1:4]`, `[4:1]` and `[-2:1]`, a write then a read at the same index must hit the same word. A block that does not write must not create a feedback loop (the v4 x defaults). `$readmemh("init.hex", m)` reads from the source file's directory with `@` addresses. Pinned in Task 20 (`OFFSET` 1/1/−2, raw `RD_ADDR`) and Task 24 (write ports with `addr`/`data` x defaults, `INIT` words, `elab_mem_bridge` on all three ranges).
5. **Generate names users probe.** Users expect `genblk1.genblk1.x` for an `if … else if` chain, `genblk3[0].w` after it, `g[-1].t`, and a user wire `genblk1` that does not rename a later block. Pinned in Task 14 (stage-1 names) and Task 17 (IR wire names `g[0].t`, `genblk3[0].w`).

**Internal risks for the reviewers** (spec §16 items 3–5, and the IR changes Peter reviews):
- **The `$proc` encoding (§16 #5, IR change B).** A body position off by one still passes a careless check. Pinned in Task 3 (every malformed body, the canonical-layout partition checks, no allocation in `verify`) and Task 21 (the bridge on real bodies).
- **Pass-W replay (§16 #4).** Pinned in Task 11 (a hook dropping one log entry gives `ODIN3_ERR_CHECK` with the exact text).
- **Case save/restore (§16 #3).** Pinned in Task 22 (nested switches with their own temporaries; the default/nolatches pair).

---

## PR 2D-1 — IR changes, pass flag, `write_rtlil`, ABI additions (v4)

### Task 1: IR change A — nine word cells (EL-31, §7.2)
**Files:**
- Modify `src/ir/cells/arith.c` (`$neg` unary with a hook; `$pow` binary, `simulate` NULL).
- Modify `src/ir/cells/logic.c` (`$xnor`, `$logic_and` and `$logic_or` binary, `$logic_not` unary, all with hooks).
- Modify `src/ir/cells/reduce.c` (`$reduce_xnor`, `$reduce_bool`, with hooks).
- Modify `src/ir/cells/shift.c` (`$shiftx` binary, `simulate` NULL).
- Modify `src/ir/cells/cells.h` (nine `extern`s) and `builtin.c` (append in point (f)'s order).
- Modify `src/backends/verilog/writer.c` (review M6): `BUILTINS` gains `$neg` (`FORM_UNARY`, `-`), `$reduce_xnor` (`~^`), `$reduce_bool` (`|`), `$logic_not` (`!`), `$xnor` (`FORM_BINARY`, `~^`), `$logic_and` (`&&`) and `$logic_or` (`||`). `$shiftx` and `$pow` are refused with point (w)'s text (superseded: generic instances, see (w)).
- Modify `tests/unit/test_ir_cells.c`, `test_sim_word.c` and `test_verilog_writer.c`.
- Modify `tools/sim-check/gen_simlib_cases.py`: `BINARY` gains `$xnor $logic_and $logic_or`, `UNARY` gains `$neg $reduce_xnor $reduce_bool $logic_not`. Regenerate `tests/unit/simlib_word_cases.h`. The existing rows change too, because the lists feed the RNG stream (review M7); the commit says so.
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
- Every `simlib_word_cases.h` row equal.
- `test_verilog_writer`:
  - a module with one `$logic_and` (`A_WIDTH` 2, `B_WIDTH` 3, `Y_WIDTH` 1) writes `assign … = … && …;`;
  - a `$shiftx` node → `INVALID_ARG` with `module 'm': cell '$c1' of type '$shiftx' has no structural Verilog form` (superseded by (w)'s amendment: the exact generic-instance text).

### Task 2: IR change C — port range and signedness (EL-24, §6.2)
**Files:**
- Modify `src/ir/module.h`: `odin3_port_spec` appends `bool has_range; int32_t msb, lsb; bool is_signed;` after `prov`, and the contract paragraph is updated.
- Modify `src/ir/module.c` (`odin3_module_add_port`).
- Modify `src/ir/celltype.h`: `odin3_port_def` appends `bool is_signed;`.
- Modify `src/ir/cells/cells.h`: `ODIN3_PORT_BIT`/`_VEC`/`_FN` gain a trailing `false`.
- Every positional `odin3_port_def`/`odin3_port_spec` initializer gains its trailing values (review M1):
  - `src/ir/cells/const.c`;
  - `tests/unit/test_{ir_netlist,ir_celltype,ir_wire,ir_check,ir_compact,sim_word,sim_build,sim_cycle,blif_writer,blif_reader,techlib_reader,json_writer,verilog_writer,dot_writer}.c`.
  The compiler finds any other.
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
- All existing `test_ir_*`, reader and writer tests pass unchanged apart from initializers.
- OOM sweep over `add_port` with a range: `NO_MEMORY`, module unchanged.

### Task 3: IR change B — the `$proc` cell (EL-33, §8.1)
**Files:**
- Create `src/ir/proc.h` and `src/ir/proc.c` (body types, pack/view, allocation-free verify, counter queries, sigspec, printer).
- Create `src/ir/cells/proc.c` (the cell type) and `tests/unit/test_ir_proc.c`.
- Modify `src/ir/value.c`: `has_payload` and `odin3_value_valid` accept `ODIN3_VAL_PROC`; `odin3_value_equal` handles it as bytes.
- Modify `include/odin3/odin3.h`: `ODIN3_VAL_PROC` appended to `odin3_value_kind`, `ODIN3_CT_PROCESS = 32` appended to the `ODIN3_CT_*` enum, each with its comment.
- Modify `src/ir/celltype.c`: raise the kind bound in `param_error`/`def_valid` from `COVER` to `PROC` and add point (g)'s `$proc`-only rule (review I14).
- Modify `src/ir/ir_internal.h` (module field `uint32_t proc_count`).
- Modify `src/ir/node.c`: the count changes on create, create_connected, delete and replace of a node whose type has the flag.
- Modify `src/ir/compact.c`: it keeps the count; tombstones copy `BODY` like any parameter.
- Modify `src/ir/cells/cells.h` and `builtin.c` (append `&odin3_cell_proc`).
- Add `ODIN3_VAL_PROC` arms to the only other `-Wswitch` sites, `src/backends/json/emit.c` and `src/backends/verilog/writer.c`; each returns `INVALID_ARG` (unreachable after Task 4's refusals).
- Modify `CMakeLists.txt`.
- Modify `docs/IR.md`:
  - a new **IR-19**: the cell, ports, parameters, the body, the `OUT`-read rule, the canonical layout, verify, and merge/delete/compact behaviour;
  - IR-19 also says that "each net on at most one `I` and one `O` pin of one `$proc`" is a property of how 2D builds nodes, not a check rule (review M22);
  - IR-9: the RTLIL view allows `$proc`; the netlist view does not;
  - IR-10: the `proc` value kind;
  - IR-12: `hier` is module-relative, made here only (review M14).
- Modify `docs/DESIGN.md`: §5.2 (`$proc` as the process cell) and §6 step 2 ("processes" = `$proc` nodes; `proc` before `opt`).

**Consumes:** Task 2 (port macros), `odin3_node_create_connected`, `odin3_net_const_value`, `odin3_net_primary`, `odin3_strbuf`.

**Produces (`proc.h`):**
- §8.1's types verbatim: `odin3_pbit` with `ODIN3_PBIT_IN`/`_OUT`/`_ANY`/`_INDEX_MASK`, `odin3_pspan`, `odin3_sync_type`, `odin3_proc_action`, `odin3_proc_case`, `odin3_proc_switch`, `odin3_proc_sync`, `odin3_proc_body`.
- `enum { ODIN3_SWITCH_FULL_CASE = 1, ODIN3_SWITCH_PARALLEL_CASE = 2 };`, `enum { ODIN3_PROC_P_I_WIDTH, ODIN3_PROC_P_O_WIDTH, ODIN3_PROC_P_BODY };`, `enum { ODIN3_PROC_PORT_I, ODIN3_PROC_PORT_O };`.
- `odin3_status odin3_proc_body_pack(odin3_arena *arena, const odin3_proc_body *body, odin3_value *out)` builds point (e)'s blob field by field. The body must already be in canonical layout; pack does not reorder.
- `bool odin3_proc_body_view(const odin3_value *val, odin3_proc_body *out)` returns false for:
  - another kind;
  - a misaligned `bits`;
  - a short or inconsistent header;
  - a count whose array overruns `len`.
- `typedef struct odin3_proc_why { char text[ODIN3_WIDTH_WHY_MAX]; } odin3_proc_why;`
- `odin3_status odin3_proc_body_verify(const odin3_proc_body *body, uint32_t i_width, uint32_t o_width, odin3_proc_why *why)` checks every §8.1 rule, point (e)'s canonical layout and point (h). It runs in O(body) with running counters and **never allocates** (review I3).
  - *Amended (Task 3, agent defaults):*
    - The widths are one argument, `odin3_proc_widths widths` (`{i_width, o_width}`), because lint (`bugprone-easily-swappable-parameters`) refuses two adjacent `uint32_t`s.
    - The "assigned" and "twice" checks cannot be O(body) without memory, so they mark positions in a fixed 8192-bit stack bitmap: one pass for up to 8192 outputs (or an lhs spanning that many), else one pass per 8192.
    - The four `ODIN3_PBIT_*` values are macros, because a C17 enumerator must fit an `int`.
    - `odin3_proc_value_text` (the RTLIL spelling of an attribute or parameter value) is added for the printer and `write_rtlil`.
    - `odin3_attr_set` refuses a `PROC` value.
- `uint32_t odin3_module_proc_count(const odin3_module *module);`
- `bool odin3_design_has_processes(const odin3_design *design);`
- `odin3_module_id odin3_design_first_proc_module(const odin3_design *design);` (0 when none)
- `odin3_status odin3_proc_sigspec(const odin3_module *module, const odin3_net_id *nets, uint32_t n, odin3_strbuf *out);` gives point (s)'s spelling. A none entry is `ANY`.
- `odin3_status odin3_proc_print(const odin3_module *module, odin3_node_id node, uint32_t indent, odin3_strbuf *out);` prints the RTLIL `process` block per §13, named per point (r), with the node's attributes first.
- `odin3_cell_proc`:
  - `{"$proc", ODIN3_GRAN_WORD, ODIN3_CT_PROCESS, {I, O}, {I_WIDTH INT 0, O_WIDTH INT 1, BODY PROC empty}, proc_verify, NULL, NULL, NULL}`.
  - `proc_verify(const odin3_value *params)` checks `O_WIDTH` ≥ 1 and `I_WIDTH` ≥ 0, views the body and calls `odin3_proc_body_verify`. On failure it logs `$proc: BODY: <why>` and returns `INVALID_ARG`.

**Tests (`test_ir_proc.c`):**

Fixture P1 is `always @(posedge clk) q <= d;` built by hand on four 1-bit **wires** `clk`, `d`, `q`, `t0` (review M5):
- pins `I` = [clk, d], `O` = [q, t0];
- pbits `[OUT|1, OUT|0, OUT|1, IN|1, OUT|0, OUT|1]`;
- actions a0 (`t0 ← q`), a1 (`t0 ← d`), a2 (`q ← t0`);
- root case with actions [0, 2);
- sync `POSEDGE IN|0` with action 2.

Cases:
- **Create and query.** `odin3_node_create_connected` → OK. `odin3_net_driver(q)` is `O` pin 0, the sinks of `d` include `I` pin 1, `odin3_module_proc_count` is 1, `odin3_design_has_processes` is true, and `odin3_check_design` FULL is OK.
- **Malformed bodies.** Each is `INVALID_ARG` with nothing created. The sink sees two lines (review M2): `$proc: BODY: <why>`, then `node_create: '$proc' rejects these parameters`. The `<why>`s, exactly:
  - `IN position 2 is out of range (I_WIDTH 2)`;
  - `action 1: lhs holds a non-OUT position`;
  - `action 0: OUT position 0 twice in one lhs`;
  - `action 0: lhs and rhs widths differ (1, 2)`;
  - `ANY outside a compare`;
  - `OUT position 2 is never assigned` (O_WIDTH 3);
  - `root case has compares`;
  - `switch 0: case 1 compare 0 has width 2; the signal has 1`;
  - `case 1: actions start at 0, expected 2` (action 0 reused by a second case);
  - `case 0: actions start at 1, expected 0` (action 0 skipped);
  - `switch 0: cases start at 2, expected 1`;
  - `switch 1: case 2 is not after its parent case 2` (a cycle through the partition);
  - `sync 0: an ALWAYS sync has a signal`;
  - `sync 0: the signal must be one IN or OUT position` (a signal of `ANY`);
  - `not a process body` (a `BITS` value; a blob 4 bytes short; a misaligned copy).
- **No allocation in `verify`.** With `odin3_util_set_alloc_fail_after(0)`, `odin3_proc_body_verify` of P1 still returns OK.
- **Point (h).** A `POSEDGE` sync on `OUT|0` (`always @(posedge a) a <= ~a`) is accepted.
- **Merge leaves the body alone.** `odin3_net_merge({keep d2, drop d})` moves `I` pin 1 to `d2`, and the `BODY` value stays byte-identical (memcmp before/after).
- **Rule 4.** A second `$add` driving `q` makes `check` report rule 4 (two drivers).
- **Compact.** A module with live P1 and a deleted copy is compacted: the live body is unchanged, and the tombstone has type `$proc` with a `BODY` equal (`odin3_value_equal`) to the deleted node's.
- **Counter.** Create 2 → 2; delete 1 → 1; `odin3_node_replace` → 1; compact → 1; delete → 0 and `has_processes` false.
- **Printer.** `odin3_proc_print` of P1 (node ID 1, indent 0) is exactly:

  ```
  process $proc$1
    assign \t0 \q
    assign \t0 \d
    sync posedge \clk
      update \q \t0
  end
  ```

  P2 adds a 1-bit wire `s`, the switch flag `FULL_CASE`, one case comparing `s` to an `I` pin on the shared `$_CONST1_` net with `t0 ← d`, and a default case. It prints `attribute \full_case 1`, `switch \s`, `case 1'1`, `assign \t0 \d`, `case`, `end` in that order.
- **`odin3_proc_sigspec`.** `[a0, a1]` of a 4-bit wire `a` → `\a [1:0]`; all four → `\a`; `[const1, ANY, x-net]` → `3'x-1`.
- **Registration rules.** A plugin or local type with `ODIN3_CT_PROCESS` or a `PROC` parameter → `INVALID_ARG` with point (g)'s text. The built-in registers.
- OOM sweep over `pack` and `create_connected`.

### Task 4: Passes and processes, writer refusals, `compact` symtab remap (EL-33 pass flag; EL-2 remap)
**Files:**
- Modify `src/passes/manager.h`: `uint8_t processes` is appended to `odin3_pass_def`, and the paragraph says what it means.
- Modify `src/passes/manager.c`: the refusal in `run_def`, before `odin3_pass_run_begin` (point (i)).
- Modify `src/passes/builtin.c` and `write_formats.c` so every `odin3_pass_def` initializer gets its value:
  - `OK`: `read_blif`, `read_techlib`, `check`, `stats`, `compact`, `hierarchy`, `write_verilog_ast`, plus point (x)'s passes when present;
  - `REFUSE`: `write_blif`, `write_json`, `write_verilog`, `write_dot`, `sim` (`sim_pass.c`).
- `compact` calls `odin3_symtab_remap` after each module.
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

*Amended (Task 4, agent defaults):*
- `processes` follows the 2D spec v4.2 (EL-33, mode P), not this plan's older text: the opt passes are `OK` when 2F lands them. DESIGN §6 step 2, IR-19's "Passes" bullet and §8.1's lists (plus §9.1's example and §19) say so.
- One helper, `odin3_design_refuse_processes(design, pass)` in `src/ir/proc.{h,c}`, logs both texts (pass name, or NULL for a writer); the manager and the four writers call it.
- `odin3_pass_register_def` also refuses `processes` > 1 (`pass_register: pass '<p>': processes must be 0 (REFUSE) or 1 (OK), not N`), as the ABI does for its slot (`odin3_pass_register: …`).
- `tests/unit/proc_fixture.{h,c}` hold P1 for `test_pass_manager` and `test_pass_outputs` (linked by `target_sources`).
- Task 3 review minors folded in: M1 (`odin3_proc_print` fails on a position on an unconnected pin: `proc_print: node N: position IN|k is on an unconnected pin`), M3 (`verify` rejects an empty action: `action N: empty`), M5 (a printer test of multi-bit chunk actions).

### Task 5: `write_rtlil` and the Yosys-0.55 CMake block (EL-48, §13; ruling C2)
**Files:**
- Create `src/backends/rtlil/writer.h`, `writer.c` and `tests/unit/test_rtlil_writer.c`.
- Modify `src/passes/write_formats.c` (`odin3_pass_write_rtlil`, `processes` OK), `builtin.h` (extern) and the built-in table.
- Modify `CMakeLists.txt`:
  - the unit test;
  - **point (p)'s `ODIN3_YOSYS055` block, created here**, with a configure-time `yosys -V` check;
  - CTest `rtlil_yosys_roundtrip` inside the block.
- Modify `docs/PASSES.md` (row `write_rtlil <file>`, 2D, RTLIL view → —) and `docs/DESIGN.md` §9 (row "RTLIL (Yosys text) | — | yes | word level with processes; the Yosys bridge for 2D–2F tests").

**Consumes:** Tasks 2–4; `odin3_attr_foreach`, `odin3_net_alias_next`, `odin3_prov_sources`, `odin3_proc_sigspec`, `odin3_proc_print`; `odin3_atomic_file_open/close` (`src/util/file.h`, review M4).

**Produces:**
- `odin3_status odin3_rtlil_text(const odin3_design *design, odin3_strbuf *out);`
- `odin3_status odin3_rtlil_write(const odin3_design *design, const char *path);` (atomic)
- `extern const odin3_pass_def odin3_pass_write_rtlil;`

Format (§13; field order as Yosys 0.55's `rtlil_backend.cc`):
- One `module \name` per module in creation order, with its attributes first.
- Live wires in ID order: `wire [width W] [upto] [offset O] [input|output|inout K] [signed] \name`, K = the 1-based port index.
- For each net that is no wire's bit: `wire \name` when the net has a name, else `wire $n<ID>`. The width is 1, and the spelling matches point (s) (review R4).
- Sigspecs are `odin3_proc_sigspec`'s.
- Cells:
  - `cell <type> <name>`, where an unnamed node is `$c<ID>`, a type's name starting with neither `$` nor `\` gets `\`, and instance nodes use their module's name;
  - `parameter \P <value>` in definition order (INT decimal, BITS `<n>'<bits>`, STRING `"…"` with escapes; `MEMID` gets a `\` prefix);
  - `connect \PORT <sig>` in port order;
  - `end`.
- Port and constant nodes are not written.
- `$proc` nodes print through `odin3_proc_print` at indent 2, after the cells.
- Then `connect <alias> <primary>`, one per alias run, in alias order.

**Tests (`test_rtlil_writer.c`):**
- Fixture: module `m` with ports `input [3:0] a` (port 1) and `output signed [0:1] y` (port 2); wire `b` `[3:0]` merged onto `a`; the four shared constant nodes; one unnamed `$add` (A = `a`, B = the constant nets for `4'b0011`, `Y_WIDTH` 2 on `y`), created last with `odin3_node_create_connected`. The test reads the `$add`'s ID k (review M12). The text contains exactly these lines in this order:
  - `module \m`
  - `  wire width 4 input 1 \a`
  - `  wire width 2 upto output 2 signed \y`
  - `  wire width 4 \b`
  - `  cell $add $c<k>`
  - `    parameter \A_SIGNED 0` (then `\B_SIGNED 0`, `\A_WIDTH 4`, `\B_WIDTH 4`, `\Y_WIDTH 2`)
  - `    connect \A \a`
  - `    connect \B 4'0011`
  - `    connect \Y \y`
  - `  end`
  - `  connect \b \a`
  - `end`
- Wires and sigspecs: a wire `[8:1]` prints `wire width 8 offset 1 \w`; a concatenation prints `{ \a [3:2] 2'x1 }`.
- **Every sync type** (review M11). One process holds syncs `POSEDGE`, `NEGEDGE`, `LEVEL0`, `LEVEL1` and `ALWAYS`, and prints `sync posedge \c`, `sync negedge \c`, `sync low \c`, `sync high \c`, `sync always`, each with its `update` lines. A `(* src = "t.v:3" *)` attribute on the node prints `attribute \src "t.v:3"` before `process`.
- `$mem`: a node prints `parameter \MEMID "\\m"`, `parameter \OFFSET 1` and `parameter \INIT 16'xxxxxxxxxxxxxxxx`.
- Two writes give identical bytes.
- CTest `rtlil_yosys_roundtrip`:
  - the unit test writes its fixtures to `build/<preset>/rtlil/*.il`;
  - Yosys 0.55 runs `read_rtlil F; write_rtlil G` and `read_rtlil F; proc; opt; write_blif H`, each exiting 0, smallest first.

### Task 6: ABI additions (v4) (EL-47; ruling I1)
**Files:**
- Modify `include/odin3/odin3.h`:
  - **`ODIN3_ABI_VERSION` stays 4**;
  - the history line gains `ABI v4 (cont., 2D): $proc processes and process counts, value kind PROC, pass processes slot` (Tasks 10 and 17 extend it);
  - add `odin3_module_get_proc_count`.
- Modify `src/api/module.c`, and `src/api/node.c` (the kind and text of `BODY`, the text through `odin3_proc_print`).
- Modify `plugins/python/odin3.py` (value-kind name `PROC`).
- Modify `tests/unit/test_abi_ir.c`, `test_abi_values.c` and `tests/tools/test_python_walk.py`.

**Consumes:** Tasks 3–4.

**Produces:**
- `odin3_status odin3_module_get_proc_count(const odin3_design *design, uint32_t module, uint32_t *count);` with the paragraph: O(1), `INVALID_ARG` as the conventions say.
- `odin3_node_get_param_kind` of `BODY` gives `ODIN3_VAL_PROC`.
- `odin3_node_get_param_text` of `BODY` gives the process text (interned, as the 1D text convention).

**Tests:**
- `odin3_abi_version()` is still 4, and `cli_version` keeps `(ABI 4)`.
- `get_proc_count` of P1's module → 1; module 99 → `INVALID_ARG`.
- Param kind of index 2 → 4.
- Param text equals Task 3's P1 text exactly.
- The Python walk prints the `$proc` node with kind `PROC` and its body text.

## PR 2D-2 — constants: values, typing, evaluator, interpreter

### Task 7: Values, operators and the Yosys 0.55 constant table (EL-12, EL-14, EL-15)
**Files:**
- Create `src/frontends/elab/elab.h`: caps, `odin3_elab_opts`, and the stage API declarations added later (Tasks 10, 17).
- Create `src/frontends/elab/elab_test.h`: `odin3_elab_limits` and `odin3_elab_test_set_limits`. Task 10 adds the stage hooks here, and Task 17 is their second user (review M16).
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
  - `odin3_status odin3_cval_to_number(odin3_arena *arena, const odin3_cval *val, odin3_ast_number *out);` gives point (bb)'s spelling: payload `width` 0 when `UNSIZED`.
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
  - `odin3_status odin3_cval_op(odin3_arena *arena, const odin3_cop_args *args, odin3_cval *out);` computes `RTLIL::const_*` semantics at `width`. Arithmetic, bitwise and unary operations extend both operands to `width` by `is_signed` (an unsized operand repeats its top bit). Comparisons extend both to their common width and give 1 bit, zero-extended to `width`. Logical operations and reductions read operands as they are and give 1 bit, zero-extended to `width`. Shifts extend `a` only; `b` is an unsigned amount, except `POW`, which reads `b`'s own `SIGNED`.
- Parts and mux:
  - `typedef struct odin3_cval_part { const odin3_cval *val; int64_t lo; uint32_t width; } odin3_cval_part;`
  - `odin3_status odin3_cval_select(odin3_arena *arena, const odin3_cval_part *part, odin3_cval *out);` (positions outside the value read x)
  - `odin3_status odin3_cval_concat(odin3_arena *arena, const odin3_cval *parts, uint32_t n, odin3_cval *out);` (MSB part first)
  - `odin3_status odin3_cval_replicate(odin3_arena *arena, const odin3_cval *val, uint32_t count, odin3_cval *out);`
  - `typedef struct odin3_cval_mux_args { const odin3_cval *cond, *a, *b; } odin3_cval_mux_args;`
  - `odin3_status odin3_cval_mux(odin3_arena *arena, const odin3_cval_mux_args *args, odin3_cval *out);`: an x `cond` merges bit by bit, equal bits kept, others x.
- Word-parallel planes for widths ≤ 64; schoolbook word loops above.

**Produces (`elab.h`, `elab_test.h`):**
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
  - Reductions: `&4'b1x11` → x; `&4'b0x11` → 0; `|4'b0x00` → x; `|4'b1x00` → 1. `&8'hFF` at `width` 8 → `8'b00000001`.
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

The two sweeps are §4.2's, each with an explicit stack: a post-order self sweep, then a pre-order context sweep in which the root's final width is `max(self, context)`.
- Comparison, reduction and logical operators are 1-bit self-determined, so in a wider context their final width is the context's. Their operands stay self-determined (spec §4.2, EL-30 `Y_WIDTH = max(final, 1)`; Yosys `genrtlil.cc` 1142–1149, 1777–1783; ruling I7).
- Diagnostics:
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
| E8: `a < t` | 8 | `<` (8, u), operands still (8, u) |
| E8: `s < t` | — | operands (8, s) |
| `t << a` | — | (4, s), `a` self (8, u) |
| `s >>> a` | — | (8, s) |
| `c ? a : t` | — | (8, u), `c` self (1, u) |
| `{a, t}` | — | (12, u) |
| `{a, 'h1}` | — | (40, u), plus the warning `x.v:1:12: warning: unsized constant in a concatenation is taken as 32 bits` |
| `{&a, b}` | — | (9, u); `&a` (1, u) |
| `{3{t}}` | — | (12, u) |
| `s[3:0]` | — | (4, u) |
| `s[2]` | — | (1, u) |
| `a[i +: 3]` | — | (3, u) |
| `$signed(a)` | — | (8, s) |
| `$unsigned(s)` | — | (8, u) |
| `t ** a` | — | (4, s), `a` self (8, u) |
| `&a`, `!a`, `a && b` | 8 | the operator (8, u); its operands self-determined (8, u) (ruling I7) |
| `~&a` | 8 | the outer `!` (8, u); the inner `&` (1, u) (v4 EL-30) |
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
- The loop hook at 16 on `for (k = 0; k < 100; k = k + 1)` in a function → `loop exceeds 16 iterations`.
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

### Task 10: Stage-1 context, the entry, options, lookup, top, pending kinds, per-case `run-reject` (EL-2, EL-3, EL-6, EL-7; point (l))
**Files:**
- Create `src/frontends/verilog/elab1.h`, `elab1.c` (context, diagnostics helpers, `odin3_elab_stage1` and `odin3_elab_run`), `elab1_lookup.c` and `elab1_top.c`.
- Create `src/frontends/elab/elab_pending.c` (point (l)'s tables) and `src/frontends/elab/elab2_stub.c`.
- Create `tests/unit/elab_test.h` (static helpers, below) and `tests/unit/test_elab_stage1_lookup.c`.
- Delete `src/frontends/verilog/elab_stub.c`.
- Modify `src/frontends/verilog/elab.h` (point (a), the ownership paragraph) and `read_verilog.c`:
  - the flags `--nolatches`, `--nomem2reg` and `--auto-blackbox` (any order, before the files) and the usage line;
  - `odin3_design_set_ast` of the parsed store before the call;
  - the call.
- Modify 2C-2's `tests/unit/test_read_verilog.c` (review I4): "without `--parse-only` → the stub's message and `INVALID_ARG`" becomes "a clean run reaches stage 2's stub: the unlocated `elaboration is not available yet (2D)` and `PARSE`".
- Modify `tools/parse-check/run-reject` and `tests/tools/test_run_reject.py`: the per-case pass/pending/fail rule of point (l) (review I4).
- Modify `src/frontends/elab/elab.h` (stage APIs) and `elab_test.h` (hooks).
- Modify `src/api/ast_abi.c` and `include/odin3/odin3.h` (`odin3_ast_get_library_units_from`, extending the v4 "(cont., 2D)" line).
- Modify `docs/PASSES.md` (the `read_verilog` row gains the three flags), `docs/DESIGN.md` §4.0 (`--auto-blackbox`; the top rule unchanged) and `CMakeLists.txt`.

**Consumes:**
- 2C-2: `odin3_read_verilog_files`, `odin3_rv_opts`, `odin3_ast_library_units_from`, the pass's option parse.
- 2A: `odin3_ast_create(…, ODIN3_AST_FORM_ELABORATED, …)`, builders, `odin3_ast_finish`, `odin3_ast_check`, `odin3_srcman_buffer_info` (a unit's library), `odin3_design_set_ast`, `odin3_design_drop_ast`, `odin3_design_find_ast`.
- 2B-1 record (`top`, `top_at`, `params`); `odin3_pass_get_options`; `odin3_celltype_find` and `_get`; `odin3_module_prov` and `odin3_prov_get` (the run of a module made by an earlier read).

**Produces:**
- `src/frontends/verilog/elab.h`: `odin3_status odin3_elab_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_ast *parsed, const odin3_elab_opts *opts);` runs EL-2 steps 1–7. Step (3) is `odin3_design_drop_ast`. It never destroys `parsed` itself (point (a)).
- `src/frontends/elab/elab.h`:
  - `odin3_status odin3_elab_stage1(odin3_pass_ctx *ctx, odin3_ast *parsed, const odin3_elab_opts *opts, odin3_ast **elab_out);`
  - `odin3_status odin3_elab_stage2(odin3_pass_ctx *ctx, odin3_ast *elab, const odin3_elab_opts *opts);` (the stub until Task 17)
  - `typedef enum odin3_elab_pend_feature { ODIN3_PEND_COMB_CALL = ODIN3_AST_KIND_COUNT, ODIN3_PEND_REGS_VAR_READ, ODIN3_PEND_MEM_WRITE, ODIN3_PEND_END } odin3_elab_pend_feature;`
  - `bool odin3_elab_pending(uint8_t stage, uint32_t kind_or_feature);`
  - `void odin3_elab_pend_report(odin3_design *design, odin3_loc loc, uint32_t kind_or_feature, bool *seen);` logs point (l)'s line once per kind per run. `seen` has `ODIN3_PEND_END` entries.
- `elab_test.h`:
  - `void odin3_elab_test_pend(uint8_t stage, uint32_t kind_or_feature, bool pending);`
  - `typedef struct odin3_elab_test_hooks { uint32_t drop_log_entry; void (*on_body_start)(void *user, odin3_ast_id module_node); void *user; } odin3_elab_test_hooks;`
  - `void odin3_elab_test_set_hooks(const odin3_elab_test_hooks *hooks);` (`drop_log_entry` 0 = off: Task 11; `on_body_start`: Task 17)
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
- **Staging at Task 10.** Stage 1 runs `collect_defs`, `select_top` and a reachability walk that resolves every `INSTANTIATION` reachable from the top (EL-6; the not-elaborated info line), then writes the `UNIT` and its `MODULE`s. Every module item is pending until Task 11 (point (l)), so the lookup tests assert their lines, which come first.
- `run-reject --elaborate`: per case **pass** / **pending** / **fail** (point (l)). The summary is `pass N, pending P, fail F`. Exit 1 on any fail, 77 when P equals the case count, else 0.
- `tests/unit/elab_test.h` (review I5):
  - `et_src { const char *path, *text; }`;
  - `et_req { const et_src *srcs; uint32_t n; const char *pre_script; const et_src *project; const odin3_elab_opts *opts; }`, where `pre_script` holds passes run on the fresh design first (e.g. `read_techlib lib/vtr.o3lib; read_project p.o3proj`), and `project` is written next to the sources when non-NULL;
  - `et_stage1(const et_req *req, et_result *out)` writes the files into a fresh directory under the build tree, runs `pre_script`, `odin3_read_verilog_files` and then `odin3_elab_stage1`, and fills `et_result { design, parsed, elab, status, dump (odin3_ast_dump of elab), log lines, errors, warnings }` through a capturing sink;
  - `et_free(et_result *r)`.

  Task 17 adds `et_read` with the same request.

**Tests (`test_elab_stage1_lookup.c`; exact lines through the sink):**
- **Lookup order.**
  - `sub` in libraries `work` and `lib2`: an instance in a `work` module resolves `work.sub`.
  - `sub` only in `lib2` and `lib3` → info `module 'sub' found in libraries 'lib2', 'lib3'; using 'lib2.sub'`.
  - A `-v` library-unit module is used only when no design unit defines the name.
  - A `primitive p` → `p.v:1:11: error: user-defined primitive 'p' is not supported`, with or without an instance.
  - `config c; … endconfig` → `configuration 'c' is not supported`.
- **Cell types.**
  - With `pre_script` `read_techlib lib/vtr.o3lib`, `single_port_ram #(.ADDR_WIDTH(10)) r (…)` resolves to the cell type.
  - `$add u (…)` → `t.v:3:3: error: unresolved_module: module '$add' is not defined`.
  - `foo u (…)` → `unresolved_module: module 'foo' is not defined`.
  - With `--auto-blackbox`, `foo` resolves to an implicit black box, with point (w)'s info line.
- **Taken names.**
  - After `read_techlib lib/vtr.o3lib`, `module adder` → `a.v:1:8: error: module 'adder' has the name of cell type 'adder' registered by an earlier read (hard cell); rename the module`.
  - After a `read_blif` declaring black box `bb`, `module bb` → `… (black box); rename the module`.
- **Top.**
  - One candidate → info `top module 'work.t' chosen: the only module no other module instantiates`.
  - The corpus `neg_ambiguous_top` sources → `odin3: error: ambiguous_top: no top given and several modules are not instantiated: 'work.a', 'work.b'`.
  - `neg_no_top` → `no_top: no top given and every module is instantiated by another`.
  - No module → `no_top: no top given and no module is defined`.
  - `--top nope` → `odin3: error: unknown_top: 'nope' is not a defined module`.
  - A project top `nope` with `top_at` (via `project` + `pre_script` `read_project`) → `p.o3proj:2:1: error: unknown_top: 'nope' is not a defined module`.
  - `--top` beats the project top.
  - `sub` in two libraries → `ambiguous_top: 'sub' is defined in several libraries: 'lib2.sub', 'lib3.sub'`; `--top lib3.sub` → OK.
  - Unreachable modules → info `not instantiated under the top, not elaborated: 'u1', 'u2'`.
  - A module instantiated only in an unselected generate branch is not a candidate.
- **Pending mechanism (ruling C3).**
  - With `odin3_elab_test_pend(1, ODIN3_AST_NET_DECL, true)`, a module with two `NET_DECL`s gives exactly one line, `t.v:2:3: error: elaboration is not available yet (2D): 'NET_DECL'`. The result is `PARSE` with no elaborated store, and `odin3_ast_check` was never handed an unhandled kind.
  - Before Task 15 lands, an `always` gives `… 'ALWAYS'` (the case is replaced when Task 15 empties that entry).
- **Ownership (review I10).** After a clean stage 1 under keep level `NONE`, `odin3_design_find_ast(…, PARSED)` gives NULL before stage 2. Under `ALL` it is held. A double drop is a no-op, and ASan sees no double free.
- **ABI.** On a run with one `-v` file after two design files → index 2.
- **Entry.**
  - `read_verilog --nolatches t.v` passes `nolatches` true.
  - `read_verilog --bogus t.v` → `read_verilog: unknown option '--bogus'`, `INVALID_ARG`.
  - A clean stage 1 is followed by the stub text and `PARSE` (until Task 17).
  - Debug `odin3_ast_check` FULL of the elaborated store passes.
- `test_run_reject.py`, over a fake `odin3`:
  - the stub text, located or not → pending;
  - all pending → 77;
  - a missing line without it → 1;
  - the summary line exact.
- CTests `verilog_reject_2d` and `verilog_ignore_2d` pass with every case pass or pending (no fail): `main` stays green.
- **OOM sweep** over stage 1 on a three-module design: `NO_MEMORY`, the parsed store intact, no elaborated store left, no leak.

### Task 11: Parameters, specializations, passes E/W, `defparam` and project push-down (EL-8, EL-9, EL-16)
**Files:**
- Create `src/frontends/verilog/elab1_params.c` (EL-16 typing of values, EL-8 precedence, cycles).
- Create `elab1_spec.c`: key bytes, `odin3_hash_bytes`, a `u64map` from hash to chain, names, the FIFO queue, the iterative-DFS acyclicity check, the depth, specialization and node caps.
- Create `elab1_expand.c`: the pass-E skeleton.
  - Environments: a scope stack, plus `u64map` `(scope << 32 | name) → odin3_elab_entry`.
  - Every declaration is recorded: nets, variables, ports, parameters, localparams, genvars, functions and tasks, with folded ranges (review C3 (a)).
  - It also covers instance and `defparam` collection and the decision log.
- Create `elab1_write.c`: the pass-W skeleton.
  - It writes point (l)'s Task-11 kinds: `MODULE`; the params `LIST` of `PARAM_DECL[localparam]{IN_HEADER}` with final values spelled per point (bb); ANSI `PORT_DECL`, `NET_DECL` and `VAR_DECL` with `NUMBER` bounds; `CONT_ASSIGN`; and `INSTANTIATION` names with named param `CONNECTION`s.
  - The `UNIT` is written in BFS order.
  - Pending kinds go to `odin3_elab_pend_report`.
- Create `tests/unit/test_elab_stage1_params.c`; modify `elab_pending.c` (remove the Task-11 kinds).

**Consumes:** Tasks 7–10; the project record's `odin3_proj_param { scope, name, value, type, at }`; `odin3_ast_number_parse` for project values.

**Produces:**
- `typedef struct odin3_elab_entry { uint8_t kind; odin3_ast_id decl; uint32_t width; bool is_signed; odin3_cval value; } odin3_elab_entry;`: `value` holds a parameter or genvar's value; a net or variable has kind `SIGNAL` with its width and sign.
- `typedef enum odin3_elab_ovr_source { ODIN3_OVR_PROJECT = 1, ODIN3_OVR_NAMED, ODIN3_OVR_DEFPARAM, ODIN3_OVR_ORDERED } odin3_elab_ovr_source;` (precedence order)
- `typedef struct odin3_elab_override { uint32_t path; uint8_t source; uint32_t order; odin3_cval value; odin3_loc at; odin3_ast_id from; } odin3_elab_override;`: `path` is a strtab ID of `W` or `u2.W` relative to the target; `order` is the stream order (first `defparam` wins).
- `typedef struct odin3_elab_decision { odin3_ast_id node; int32_t value; } odin3_elab_decision;`
- `typedef struct odin3_elab_spec_req { uint32_t def; const odin3_elab_override *ovr; uint32_t n_ovr; uint32_t depth; } odin3_elab_spec_req;`
- `odin3_status odin3_elab1_params(odin3_elab1 *e1, const odin3_elab_spec_req *req, odin3_cval *values, odin3_elab_override *pending, uint32_t *n_pending);`
  - It evaluates the definition's `parameter`s lazily in declaration order, with overrides by precedence (EL-8) and converted per EL-16 through `odin3_cval_to_param` (`cval.h`, PR 2D-2's final review: the one copy of the four-mode rule and its equal-width `UNSIZED` quirk; Tasks 12 and 14 call it too). A `STRING` value's node and EL-9 name take its bytes from `odin3_cval_string_bytes`.
  - The evaluator's per-level typers live until `odin3_ceval_destroy` (counted in its byte budget): use one `odin3_ceval` per specialization, not one per run.
  - It returns the pending (deeper) overrides.
  - Errors are §9.1's (localparam, unknown, too many, mixed, depends on itself, not constant, P4).
- `odin3_status odin3_elab1_spec_get(odin3_elab1 *e1, const odin3_elab_spec_req *req, uint32_t *spec);` finds or creates; on create it pushes back and names per EL-9 and point (q). Caps: the specialization text of point (aa).
- `odin3_status odin3_elab1_expand(odin3_elab1 *e1, uint32_t spec);`
- `odin3_status odin3_elab1_write(odin3_elab1 *e1, uint32_t spec);`
  - It replays the log in order. A log not consumed exactly is `ODIN3_ERR_CHECK` with `stage 1: decision log of 'm' not replayed (N of M entries)`, a 2D bug (§16 item 4).
  - The elaborated-node cap applies at every `make`.
- `odin3_status odin3_elab1_run_queue(odin3_elab1 *e1, uint32_t top_def);` (the §3.4 algorithm)
- Every elaborated node made from one parsed node copies its `{loc, end}`. A node stage 1 creates (a genvar `NUMBER`, a resolved `CONNECTION`, a folded bound) takes the range of the node that caused it (EL-4).

**Tests (`test_elab_stage1_params.c`; names and values asserted on the elaborated dump):** `module m #(parameter W = 1) (input [W-1:0] d);`, instantiated from `top`:
- **Precedence (Review Focus 1).**
  - `m #(.W(5)) u1 (); defparam u1.W = 7;` → `u1` is `INSTANTIATION` `m$W=5`, plus `t.v:3:10: warning: defparam of 'u1.W' is overridden by the named parameter assignment of 'u1'`.
  - `defparam u3.W = 8; defparam u3.W = 9;` → `m$W=8`.
  - `m #(4) u4 (); defparam u4.W = 6;` → `m$W=6` (divergence 7).
  - Project `param top.u1.W 3` (through `et_req.project` and `pre_script` `read_project p.o3proj`) → `m$W=3`.
  - `m u5 ();` and `m #(.W(1)) u6 ();` → both `m`, one `MODULE` in the `UNIT`.
- **Sharing by resolved value.** On `parameter integer W`, `#(.W(8))` and `#(.W(32'sd8))` share one specialization. On an untyped `W`, `#(.W(8))` and `#(.W(4'd8))` → `m$W=8` and `m$W=4'b1000`.
- **Names.**
  - String `S = "ab"` overridden with `"x\"y"` → `m$S="x\"y"`.
  - `parameter signed [7:0] P` with −16 → `m$P=8'sb11110000`.
  - Untyped `'hFFFFFFFF` → `m$P=32'b11111111111111111111111111111111`.
  - A 200-bit override → `m$h` plus 16 hex digits.
  - A user module `m$W=5` already defined → the specialization is `m$W=5$2`.
  - A registered cell type named `m$W=6` (via `pre_script`) → `m$W=6$2`.
- **Errors (exact).**
  - `#(.L(1))` on a `localparam L` → `'L' is a localparam of module 'm' and cannot be overridden`.
  - `#(.X(1))` → `module 'm' has no parameter 'X'`.
  - `#(1, 2, 3)` on two parameters → `too many parameter values for module 'm' (3 given, 2 parameters)`.
  - `#(1, .W(2))` → `parameter assignment of instance 'u' mixes ordered and named values`.
- **Dependencies (review C3 (a)).**
  - `parameter A = B + 1, B = 2` → A = 3.
  - `parameter A = B, B = A` → `parameter 'A' depends on itself` at `A`.
  - `wire w; parameter A = w;` → `parameter 'A' is not constant (it reads 'w')`, with `w` found in pass E's environment.
- **EL-16 types and the point (bb) spellings.**
  - `parameter [7:0] P = 9'h1FF` → `BIN` width 8, unsigned, `8'hFF`.
  - `parameter signed P = 4'b1000` → `BIN` width 4, `SIGNED`.
  - `localparam [31:0] Q = 'hFFFFFFFF` → `HEX` width 0 (unsized).
  - `parameter W = 1` default → `DEC` width 0, `SIGNED`.
  - `localparam [35:0] R = 'hFFFFFFFF` → `BIN` width 36.
  - `parameter real r = 1.0` → `parameter type 'real' is not supported`.
- **Declarations.** `wire [W-1:0] w;` in `m$W=5` → `RANGE(NUMBER.dec 4, NUMBER.dec 0)`, both `SIGNED` width 32.
- **Push-down.**
  - `defparam u_mid.u_leaf.W = 4;` → `mid$u_leaf.W=4` and `leaf$W=4`.
  - `defparam top.u.W = 1;` written in a child → `defparam target 'top.u.W' does not name a parameter of an instance below this module`.
  - A `defparam` naming its own module's parameter → the same error.
  - Project `param top.u_wide.W 8'hFF` pushes down; `param top.u_wide.X 1` → `p.o3proj:4:1: error: param 'top.u_wide.X' does not name a parameter`.
  - The corpus `param_instance` values: its `ref.cmd` parameters equal.
- **Recursion and caps (point (aa)).**
  - `ping`/`pong` instantiating each other, with `--top ping` → `recursive instantiation of 'ping' (through 'pong')`.
  - A six-module distinct chain with the depth hook at 4 → `instance hierarchy deeper than 4 at 'u'; recursive instantiation of 'a4'?`.
  - The specialization hook at 2 with three specializations → `t.v:6:3: error: elaboration makes more than 2 specializations`.
  - The node hook at 50 → `elaboration needs more than 50 elaborated nodes`, fatal.
- **Store shape.** `UNIT` named `top`, `top` first, then BFS order. Each `INSTANTIATION` has one named `CONNECTION` per parameter, with its final value.
- **Replay (internal risk §16 #4).** `odin3_elab_test_set_hooks({.drop_log_entry = 1})` → `ODIN3_ERR_CHECK` and `stage 1: decision log of 'm' not replayed (1 of 2 entries)`.
- OOM sweep.

### Task 12: Instances, connections, arrays, black boxes and cell types (EL-10, EL-11, EL-8 cell-type targets)
**Files:** Create `src/frontends/verilog/elab1_inst.c` and `tests/unit/test_elab_stage1_inst.c`; modify `elab1_expand.c`, `elab1_write.c` and `elab_pending.c` (ordered connections, arrays and `GATE_DECL` landed).

**Consumes:** Tasks 8–11. Connection expressions are typed with a stage-1 `odin3_ctype_host` over pass E's environment (Task 11's entries).

**Produces:**
- `typedef struct odin3_elab_inst { odin3_ast_id instantiation, instance; uint32_t scope; odin3_elab_res target; uint32_t spec; } odin3_elab_inst;`
- `odin3_status odin3_elab1_connect(odin3_elab1 *e1, const odin3_elab_inst *inst);` resolves connections to one named `CONNECTION` per port in port order, applying EL-10's rules, errors and the unnamed-position warning.
- `odin3_status odin3_elab1_inst_array(odin3_elab1 *e1, const odin3_elab_inst *inst);` expands `u[hi:lo]` to `u[hi]`…`u[lo]` with `SELECT` slices (gates use P = 1).
- `odin3_status odin3_elab1_celltype_params(odin3_elab1 *e1, const odin3_elab_inst *inst);` checks overrides by name or order against the type and converts them by kind. Stage 2 infers the rest.
- `odin3_status odin3_elab1_blackbox(odin3_elab1 *e1, uint32_t def);`: `(* blackbox *)` modules get per-specialization ports and their body is ignored; implicit black boxes take the union of named ports, `inout`, with widths from the first instance.
- The instance cap uses point (aa)'s text.

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
- **Arrays.** `wire [7:0] bus8; wire [1:0] q2; m2 u[3:0] (.d(bus8), .q(q2))` with `d` 2 bits:
  - instances `u[3]`, `u[2]`, `u[1]`, `u[0]` in that order;
  - `u[3].d` = `bus8[7:6]`, `u[0].d` = `bus8[1:0]`, and the 2-bit `q2` goes whole to each;
  - a 3-bit connection → `connection of 'd' to instance array 'u' is 3 bits; expected 2 or 8`;
  - `and g[1:0] (y2, a2, b2)` → `g[1]` with `y2[1]`, `a2[1]`, `b2[1]`.
- **Outputs.** An output connected to `a + b` → `output port 'q' of instance 'u' must be connected to a net`; to `4'd3` → `output port 'q' of instance 'u' is connected to a constant`.
- **Black boxes.**
  - `(* blackbox *) module bb(input [3:0] a, output y); assign y = &a; endmodule` → info `body of black box 'bb' is ignored`, connections as written, no `MODULE` for `bb` in the `UNIT`.
  - A parameterized black box with two override values → two names per EL-9.
- **Cell types** (with `pre_script` `read_techlib lib/vtr.o3lib`):
  - `single_port_ram #(.ADDR_WIDTH(10)) ram (…)` → the params `LIST` holds `(ADDR_WIDTH, 10)`.
  - `#(.FOO(1))` → `cell type 'single_port_ram' has no parameter 'FOO'`.
  - `defparam ram.DATA_WIDTH = 8;` → adds `(DATA_WIDTH, 8)`.
  - Project `param top.ram.ADDR_WIDTH 12` → wins.
  - `#(.ADDR_WIDTH(4'bx010))` → `value of parameter 'ADDR_WIDTH' of cell type 'single_port_ram' is not an integer`.
- **`--auto-blackbox`.**
  - `foo u1 (.a(x), .b(y)); foo u2 (.a(x2), .c(z));` → black box `foo` with ports `a b c`, all `inout`.
  - `foo u3 (x)` → `implicit black box 'foo' needs named connections`.
  - A 4-bit first `.a` and then an 8-bit one → point (w)'s error.
- The instance hook at 4 with an 8-element array → `t.v:5:3: error: elaboration makes more than 4 instances`, fatal.
- OOM sweep.

### Task 13: The binder and EL-4 validation (EL-5, EL-4)
**Files:** Create `src/frontends/elab/bind.c`, `src/frontends/elab/elab_internal.h` (binding part) and `tests/unit/test_elab_bind.c`; modify `tests/unit/elab_test_tree.h` (elaborated-store builders `eb_module`, `eb_net`, `eb_var`, `eb_localparam`, `eb_gen_block`, `eb_named_block`, `eb_function`, `eb_task`, `eb_assign`, `eb_always`, `eb_instance`).

**Consumes:** the 2A symbol table (`odin3_symtab_create`, `odin3_scope_new`, `odin3_symbol_new`, `_set_value`, `_set_unpacked`, `_add_assign`, `odin3_symtab_lookup`, `odin3_scope_child`); Task 7 (localparam values).

**Produces:**
- `typedef struct odin3_elab_binding { odin3_sym_id *sym_of; uint32_t n; odin3_u64map *scope_of; } odin3_elab_binding;`
- `odin3_status odin3_elab_bind(odin3_ast *elab, odin3_elab_binding *out);` is one pre-order sweep.
  - Its §2.5 errors are located and capped at 20; it returns `PARSE` after reporting.
  - It also checks the **EL-4 store shape** for the rows 2A's rule 7 does not cover (review I10). A violation is a misuse of the seam: located `elaborated form: <rule>` and `INVALID_ARG`, before anything reaches the IR. The rules:
    - `'WHILE' outside a function`
    - `system function '$x' (only $signed and $unsigned are allowed)`
    - `system task '$x' outside an initial block`
    - `edge and level events in one event list`
    - `'TIMING_STMT' outside an always`
    - `intra-assignment control`
    - `a count that is not a NUMBER of 0 or more`
    - `a select bound or width that is not a NUMBER`
    - `instance 'u' does not connect every port of 'm' once, in port order`
    - `port 'p' without a net kind`
    - `parameter 'P' is not a localparam with a NUMBER or STRING value`
    - `unnamed block 'b' with declarations`
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
- **EL-4 shape.** One hand-built store per rule above gives its exact line and `INVALID_ARG`, and creates no IR (module count 0).
- 10,000 nested `GEN_BLOCK`s bind without recursion.
- OOM sweep: `NO_MEMORY`, the symtab destroyed with the store, no leak.

## PR 2D-4 — stage 1: generate, declarations, diagnostics; the handover expectations

### Task 14: Generate, scope names and references (EL-19, EL-20, EL-21, EL-22)
**Files:** Create `src/frontends/verilog/elab1_gen.c` and `tests/unit/test_elab_stage1_gen.c`; modify `elab1_expand.c`, `elab1_write.c`, `elab1.h` and `elab_pending.c` (generate kinds landed).

**Consumes:** Tasks 9–12 (constant conditions, the decision log, push-down for scoped `defparam`).

**Produces:**
- `typedef struct odin3_elab_scope { uint8_t kind; uint32_t name; int32_t index; bool has_index; uint32_t parent; uint32_t counter; } odin3_elab_scope;`. Each scope's set of `genblk*` names seen so far is internal to `elab1_gen.c`, copied from the parent on entry (review M13).
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
  - Each unnamed block of that construct is named `genblk<c>`, padded `genblk0<c>`, `genblk00<c>`, … while the name is in the scope's seen set.
  - A user declaration whose name starts with `genblk` enters the set when the walk reaches it.
  - Entering a block starts a fresh counter at 0 with a copy of the set.
  - The else branch of `if … else if …` is a block of its own, named like the then-block.
  - Bare items are wrapped as blocks.

**Tests (`test_elab_stage1_gen.c`; the elaborated `GEN_BLOCK` names, indices and nesting):**
- **The review probe (Review Focus 5).** `localparam A = 0, B = 1; if (A) begin wire x; end else if (B) begin wire x; end if (B) assign y = a; genvar i; for (i = 0; i < 1; i = i + 1) begin wire w; end` gives:
  - `GEN_BLOCK genblk1` holding `GEN_BLOCK genblk1`, which declares `x` (path `genblk1.genblk1.x`);
  - `GEN_BLOCK genblk2` (the bare `if`);
  - `GEN_BLOCK genblk3` with index `NUMBER.dec 0` (point (bb)), declaring `w`.
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
  - `if (w)` with `w` a wire → `generate condition is not constant (it reads 'w')` (review M10).
  - `case (W) 1, 2: … default: … endcase` with W = 2 → item 0; with W = 5 → default; an item `2'b1x` never matches; no match and no default → nothing.
  - A `GEN_CASE` item reading wire `w` → `case item in a generate case is not constant (it reads 'w')`.
  - `if (1'bx)` → the else branch.
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
- Modify `elab1_expand.c`, `elab1_write.c` and `elab_pending.c` (Task 15's kinds landed).
- Modify `docs/specs/2026-10-09-2A-ast-design.md` §4.6 (a pointer to EL-4 for the refined elaborated form, including the unsized `PARAM_DECL` value; §19).

**Consumes:** Tasks 9–14; Task 11's environment entries (already holding every declaration).

**Produces:**
- `odin3_status odin3_elab1_ports(odin3_elab1 *e1, uint32_t spec);` merges non-ANSI declarations into one `PORT_DECL{IN_HEADER}` per port in header order:
  - the `NET` field is never 0; `DT` is `reg`, `integer` or none; `SIGNED`;
  - `RANGE` bounds are point (bb) `NUMBER`s;
  - empty positions are recorded for EL-10.
- `odin3_status odin3_elab1_decls(odin3_elab1 *e1, uint32_t scope, odin3_ast_id item);` completes Task 11's declarations: no `STRENGTH`/`DELAY`/`VECTORED`/`SCALARED`; initialisers kept.
- `odin3_status odin3_elab1_implicit(odin3_elab1 *e1, uint32_t spec);`: implicit nets per EL-25, first in the `MODULE`'s items, in first-use order, of the `` `default_nettype `` kind in effect.
  - *Carry from Task 13's review:* define the elaborated form's marker of the implicit nets' `NET_DECL` (for example a `NET_DECL` flag or a synthesized `ATTR`) in EL-4, **and** make the binder set `ODIN3_SYM_F_IMPLICIT` from it (EL-5 lists the flag; nothing sets it yet).
- `uint8_t odin3_elab1_nettype_at(const odin3_elab1 *e1, odin3_ast_id module_node);`: the last `DIRECTIVE[default_nettype]` before the module in stream order, reset by `` `resetall ``.
- `odin3_status odin3_elab1_subroutine(odin3_elab1 *e1, odin3_ast_id decl, uint32_t scope);`: non-ANSI inputs move to the ports `LIST`, one declarator each; locals go to `LIST`.
- `ALWAYS`, `INITIAL` and every statement kind are written per EL-4.
- Expression normalisation in pass W (point (bb) spellings):
  - genvar reads, `$clog2(…)`, `REPEAT` counts, `REPLICATE` counts, and `SELECT` `[m:l]` bounds and `+:`/`-:` widths become `NUMBER`s;
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
- **Constant folds (point (bb)).**
  - `wire [$clog2(16)-1:0] w;` → `RANGE(NUMBER.dec 3, NUMBER.dec 0)`, `SIGNED` width 32.
  - `repeat (N)` with N = 3 → `NUMBER.dec 3`.
  - `{N{a}}` → a `NUMBER` count.
  - `{-1{a}}` → `replication count -1 is negative`.
  - `{2'bx1{a}}` → `replication count has x or z bits`.
  - `{0{a}}` alone → `zero-width concatenation`.
  - `wire [w:0] x;` with `w` a wire → `range bound is not constant (it reads 'w')`.
  - `a[P+1:P]` with `P` = 2 → `SELECT(…, NUMBER.dec 3, NUMBER.dec 2)`; `a[i +: W]` → the width a `NUMBER`.
  - `(* weight = P * 2 *)` → `ATTR` with `NUMBER`; `(* k = w *)` → `attribute value is not constant (it reads 'w')`.
- **Genvar reads.** A genvar read inside a loop body → `NUMBER.dec` of the iteration's value.
- **M11 across kinds.** `wire x; reg x;` → `'x' is already declared (first at t.v:2:8)`.
- The elaborated store matches EL-4 for every kind in the test (check FULL and Task 13's shape check).
- OOM sweep.

### Task 16: REJECT/IGNORE diagnostics, the error cap, the handover expectations (EL-42, EL-43, EL-44; §14.4)
**Files:**
- Create `src/frontends/verilog/elab1_diag.c` and `tests/unit/test_elab_stage1_diag.c`.
- Modify `elab_pending.c`: every remaining stage-1 kind lands, and the stage-1 table is empty.
- Modify `tests/verilog/reject-2d/*.err` and `tests/verilog/ignore-2d/*.warn` only where 2C's text differs from §9 (conflict note 10).

**Consumes:** Tasks 10–15. The rows are §9.1 and §9.2. A4, T9 and T10 are stage 2's (Tasks 19 and 24).

**Produces:**
- `odin3_status odin3_elab1_reject(odin3_elab1 *e1, odin3_ast_id node);` covers the stage-1 REJECT rows at the nodes §9.1 names: L9, M11 (other than modules), N17, N18, E20, E21, T6, T12, T13, T14, T15, T16, T19, F4, K12, Q6, Q7, S2, N15/P4, F2.
- A mixed edge/level event list is rejected in stage 1, so the elaborated form holds none (EL-4): `edge and level events in one event list are not supported`. Task 21 keeps the check for adapter stores (with Task 13's shape check).
- `odin3_status odin3_elab1_ignore(odin3_elab1 *e1, odin3_ast_id node);` covers the ten IGNORE rows' warnings, once per parsed node (point (v)).
- The cap: the 21st error is replaced by `too many errors; elaboration stopped after 20`, and stage 1 stops.
- Stage 1 resumes at the next item or module. Dependents of a failed item are skipped silently.

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
- **Stage-1 table empty.** `odin3_elab_pending(1, k)` is false for every kind. Every `tests/verilog/coverage/*.v` file runs stage 1 without the not-available line.
- **CTests (expectations flipped here).**
  - `verilog_reject_2d` passes with exactly A4, T9 and T10 pending; they are stage 2's, and until Task 17 the unlocated stub text follows stage 1.
  - `verilog_ignore_2d` passes with none pending.

## PR 2D-5 — stage 2: modules, wires, expressions, drivers, memories

### Task 17: Stage-2 context, modules, ports and wires, provenance, attributes, keep levels, byte budget, `--elab-only`, `odin3_ast_elaborate` (EL-2 steps 4–7, EL-23, EL-24, EL-45, EL-46, EL-47)
**Files:**
- Create `src/frontends/elab/elab2.c` (context, module order, the steps (1)–(8) driver, summary, keep-level hand-over) and `elab2_wires.c` (wires, ports, provenance, attributes).
- Create `src/api/elab_abi.c` and `tests/unit/test_elab_stage2_wires.c`.
- Delete `src/frontends/elab/elab2_stub.c`.
- Modify `elab_internal.h`, `elab.h`, `elab_test.h` (the `on_body_start` hook used) and `elab_pending.c`:
  - declarations, ports and scope kinds landed;
  - assignments (Task 18), instances and gates (Task 19), the process kinds and the three features (Tasks 21–24) stay pending (point (l)).
- Modify `src/frontends/verilog/elab1.c` (`odin3_elab_run` steps 4–7).
- Modify `src/cli/main.c` (`--elab-only`) and `tests/cli/test_cli.sh` (`cli_elab_only`, `cli_elab_only_with_script`).
- Modify `include/odin3/odin3.h`:
  - `odin3_elab_options` and `odin3_ast_elaborate`, extending the v4 "(cont., 2D)" line;
  - the failed-read note on `odin3_design_run_script`'s paragraph: "a design whose `read_verilog` failed holds valid but incomplete modules and is fit only for destruction" (§2.3, ruling 10; review I10).
- Modify `docs/PASSES.md` (the `read_verilog` row: elaborates; `--elab-only`; keep levels) and `CMakeLists.txt`.

**Consumes:** Task 2 (`odin3_port_spec` range/sign); Task 13; `odin3_module_create`, `odin3_module_add_port`, `odin3_wire_create`, `odin3_attr_set`, `odin3_prov_source`, `odin3_srcman_srcloc`, `odin3_symbol_set_ir`, `odin3_design_set_top`, `odin3_design_set_ast`, `odin3_design_keep_ast`, `odin3_celltype_find`.

**Produces:**
- Signals:
  - `typedef uint32_t odin3_sigbit;` is a net ID, `ODIN3_SB_CONST | odin3_bit`, or `ODIN3_SB_ANY`, with `enum { ODIN3_SB_CONST = 1U << 31, ODIN3_SB_ANY = (1U << 31) | 4U };`. This one encoding serves expressions and the process generator (review M21).
  - `typedef struct odin3_sig { odin3_sigbit *bits; uint32_t width; bool is_signed; } odin3_sig;` (LSB first, in the stage-2 scratch arena)
- Context and module emission:
  - `typedef enum odin3_elab2_type { ODIN3_E2T_ADD, ODIN3_E2T_SUB, ODIN3_E2T_MUL, ODIN3_E2T_DIV, ODIN3_E2T_MOD, ODIN3_E2T_POW, ODIN3_E2T_AND, ODIN3_E2T_OR, ODIN3_E2T_XOR, ODIN3_E2T_XNOR, ODIN3_E2T_NOT, ODIN3_E2T_NEG, ODIN3_E2T_SHL, ODIN3_E2T_SHR, ODIN3_E2T_SSHR, ODIN3_E2T_SHIFTX, ODIN3_E2T_EQ, ODIN3_E2T_NE, ODIN3_E2T_LT, ODIN3_E2T_LE, ODIN3_E2T_GT, ODIN3_E2T_GE, ODIN3_E2T_REDUCE_AND, ODIN3_E2T_REDUCE_OR, ODIN3_E2T_REDUCE_XOR, ODIN3_E2T_REDUCE_XNOR, ODIN3_E2T_REDUCE_BOOL, ODIN3_E2T_LOGIC_NOT, ODIN3_E2T_LOGIC_AND, ODIN3_E2T_LOGIC_OR, ODIN3_E2T_MUX, ODIN3_E2T_TRIBUF, ODIN3_E2T_MEM, ODIN3_E2T_PROC, ODIN3_E2T_CONST0, ODIN3_E2T_CONST1, ODIN3_E2T_CONSTX, ODIN3_E2T_CONSTZ, ODIN3_E2T_COUNT } odin3_elab2_type;` lists exactly the built-in types 2D creates (review R6).
  - `typedef struct odin3_elab2 odin3_elab2;` caches one `odin3_celltype_id` per `odin3_elab2_type`, filled once at create by `odin3_celltype_find` (review M20).
  - `odin3_celltype_id odin3_elab2_type_id(const odin3_elab2 *e2, odin3_elab2_type type);`
  - `odin3_status odin3_elab2_create(odin3_pass_ctx *ctx, odin3_ast *elab, const odin3_elab_opts *opts, odin3_elab2 **out);`
  - `void odin3_elab2_destroy(odin3_elab2 *e2);`
  - `odin3_status odin3_elab2_ports(odin3_elab2 *e2, odin3_ast_id module_node);` creates the module and its ports, recording each port symbol's `ir_wire` (`odin3_symbol_set_ir`). It runs for every `MODULE` before any body. A module whose ports could not be built stops stage 2.
  - `odin3_status odin3_elab2_module(odin3_elab2 *e2, odin3_ast_id module_node);` runs §6.6's steps (1)–(8). Pending constructs go to `odin3_elab_pend_report` and are skipped.
  - `odin3_status odin3_elab2_wires(odin3_elab2 *e2, odin3_ast_id module_node);` is step (1). Names follow EL-23, `odin3_symbol_set_ir` is called for each symbol, and the net-bits cap of point (aa) applies.
- Helpers:
  - `odin3_status odin3_elab2_const_net(odin3_elab2 *e2, odin3_bit bit, odin3_net_id *out);` gives the module's shared `$_CONST0_/1/X/Z_` net, created on first use (IR-4).
  - `typedef struct odin3_elab2_prov_req { odin3_ast_id node, call; uint32_t scope; } odin3_elab2_prov_req;`
  - `odin3_status odin3_elab2_prov(odin3_elab2 *e2, const odin3_elab2_prov_req *req, odin3_prov_id *out);` makes a SOURCE record per EL-45: locs = node, then call when inlined; `ast` = node; `hier` = `<spec name>/<scope path>`.
  - `odin3_status odin3_elab2_attrs(odin3_elab2 *e2, odin3_ast_id node, odin3_objref obj);` applies 2A §7: none → INT 1, `NUMBER` → BITS, `STRING` → STRING.
  - `void odin3_elab2_error(odin3_elab2 *e2, odin3_loc loc, const char *fmt, ...);` and `void odin3_elab2_warn(odin3_elab2 *e2, odin3_loc loc, const char *fmt, ...);` (cap 20 with `too many errors; elaboration stopped after 20`; warn once per loc)
  - `odin3_status odin3_elab2_budget(odin3_elab2 *e2);` is EL-46's byte budget, checked at each module boundary and every 2^16 IR objects, with §11's per-object estimate.
- Real stage 2 replaces the stub. `odin3_elab_stage2` runs bind, then `ports` for all, then `module` for each. On success it calls `odin3_design_set_top` (the `UNIT`'s name; **not** called after any stage-2 error) and logs the info summary `read_verilog: elaborated top 'T': M modules (S specializations), P processes, C cells, N nets, t1 + t2 ms`.
- Step (6) (review I10): `odin3_elab_run` hands the elaborated store and its symtab to the design with `odin3_design_set_ast` when the keep level is `ELABORATED` or `ALL`; otherwise it destroys the store at the end of `read_verilog`.
- ABI:
  - `typedef struct odin3_elab_options { uint32_t top; uint32_t top_at; bool nolatches, nomem2reg, auto_blackbox; uint32_t reserved[4]; } odin3_elab_options;`
  - `odin3_status odin3_ast_elaborate(odin3_design *design, odin3_ast *ast, const odin3_elab_options *opts);` (point (b)), with the paragraph and the run-ID caveat. `reserved` must be zero, else `INVALID_ARG`.
  - *Carry from Task 13's review (m4):* on this path `odin3_ast_check` rule 7 runs **in every build** (not only Debug or `--check`) before `odin3_elab_bind`, since an adapter's store is untrusted; a violation is `INVALID_ARG`. The binder checks only the rows rule 7 does not (EL-4, v4.9).
- CLI: `odin3 --elab-only a.v b.v` ≡ `-p "read_verilog a.v b.v; stats"`. With a script it fails with point (w)'s text.

**Tests (`test_elab_stage2_wires.c`; `et_read(const et_req *req, et_result *out)` added to `elab_test.h` runs the whole pass):**
- **Wires.** `wire [0:3] w; wire [-1:2] n; reg signed [7:0] r; integer i;` → IR wires `w` `[0:3]` unsigned, `n` `[-1:2]`, `r` `[7:0]` signed, `i` `[31:0]` signed. Each symbol's `ir_module`/`ir_wire` is set (review I10).
- **Scoped names (Review Focus 5).**
  - `g[0].t`, `blk.count`, `$unnamed_block$1.tmp`.
  - Task 14's probe gives wires `genblk1.genblk1.x` and `genblk3[0].w`.
  - `g[-1].t` exists.
- **Ports (IR change C).** `input signed [0:7] a` → port wire `[0:7]` signed. `input b` → `scalar`.
- **Ports before bodies.** `top` instantiating `sub` twice with different parameters: an `on_body_start` hook sees every module's port count final at the first body. The `INSTANTIATION` itself is pending until Task 19, and the test asserts only the hook's observation.
- **Provenance.**
  - Wire `w`'s record has `locs[0]` = its declarator (`t.v:2:15`–`2:16`), `ast` = the declarator's elaborated ID, `hier` = `top`.
  - `g[1].t` has `hier` `top/g[1]`.
  - A wire of `sub$W=8`'s `genblk1` has `sub$W=8/genblk1`.
- **Attributes.** `(* keep *) wire w;` → wire attribute `keep` INT 1; `(* src = "x" *)` → STRING; `(* init = 4'b1010 *)` → BITS.
- **Top.** For a one-module design with wires only, `odin3_design_top` is `top`'s module, and the summary line matches `^read_verilog: elaborated top 'top': 1 modules \(0 specializations\), 0 processes, 0 cells, [0-9]+ nets, [0-9.]+ \+ [0-9.]+ ms$`. Task 19 repeats it with three modules.
- **Keep levels (review I10).**
  - `NONE` → no store held after the pass.
  - `ELABORATED` → the elaborated store held, with its symtab, and `write_verilog_ast` of it prints the elaborated `UNIT`.
  - `ALL` → both stores held.
- **Second read (moved from Task 10; review R3).** `pre_script` `read_verilog first.v`, then sources that also define `m` → `duplicate_module: module 'm' is already defined by an earlier read (run 1)`. The first read now creates IR module `m`, so the check of Task 10's `collect_defs` can fire.
- **Failure path (review M10).** A port the IR refuses (hook) stops stage 2 after that module; no body runs and `odin3_design_top` stays unset. The error-cap test is Task 19's (review R1).
- **Caps (point (aa)).** The net-bits hook at 64 with a `wire [99:0] w;` → `t.v:2:15: error: elaboration needs more than 64 net bits`, fatal.
- **Budget.** The hook at 65536 bytes → `elaboration needs more than 65536 bytes (the budget is 60% of physical memory)`, `PARSE`, IR check passes.
- **ABI.**
  - A store built by hand through `odin3_design_new_ast(design, ODIN3_AST_FORM_ELABORATED, run, …)` and the ABI builders → `odin3_ast_elaborate` gives the module with its wires.
  - A parsed-form store → `INVALID_ARG` and `odin3_ast_elaborate: the store is not elaborated`.
  - Nonzero `reserved` → `INVALID_ARG`.
- **Pending.** An `ALWAYS` (until Task 21) → `t.v:3:3: error: elaboration is not available yet (2D): 'ALWAYS'`, the rest of the module emitted, `PARSE`.
- **Determinism.** Two runs give identical `write_rtlil`.
- **CLI.** `cli_elab_only` logs `stats:`. `cli_elab_only_with_script` fails with the text.
- OOM sweep over stage 2 of a three-module design: `NO_MEMORY`, IR check passes, no leak.

### Task 18: Expressions and selects to word cells (EL-30, EL-31, EL-32 reads)
**Files:** Create `src/frontends/elab/elab2_expr.c`, `elab2_select.c`, `elab2_assign.c` and `tests/unit/test_elab_stage2_expr.c`; modify `elab_internal.h` and `elab_pending.c` (`CONT_ASSIGN`/`NET_ASSIGN` landed in stage 2; review R1).

**Consumes:** Tasks 1, 8, 9, 13, 17.

**Produces:**
- `typedef struct odin3_elab2_reads { odin3_status (*bit)(void *user, odin3_net_id net, odin3_sigbit *out); void *user; } odin3_elab2_reads;` is the `rv` substitution hook (Task 21); NULL outside processes.
- `typedef struct odin3_elab2_expr_req { odin3_ast_id expr; odin3_etype context; const odin3_net_id *dest; const odin3_elab2_reads *reads; } odin3_elab2_expr_req;`: `dest` is NULL, or `context.width` nets the outermost cell drives by construction (EL-28).
- `odin3_status odin3_elab2_expr(odin3_elab2 *e2, const odin3_elab2_expr_req *req, odin3_sig *out);` types the expression and emits it post-order. All-constant operands fold through `odin3_cval_op`.
  - *Carry from Task 13's review (m1):* an `IDENT` or `HIER_NAME` that names a generate block or named block binds to its `BLOCK` symbol (the binder rejects instance, function and task names with `'u' is not a net, variable or parameter`). Yosys 0.55 makes such a name an implicit net (`Warning: Identifier '\g' is implicitly declared`); the binder's symbol table cannot hold a net beside the block of one name, so Task 18 (or Task 15, declaring it in stage 1) decides: an implicit net with the warning, as Yosys, or an error recorded as a §17 divergence.
  - An unsized `localparam` read takes `UNSIZED` from its declarator's `NUMBER` (payload width 0), because the symtab value holds only bits and sign (review I11).
- `odin3_status odin3_elab2_assign(odin3_elab2 *e2, odin3_ast_id net_assign);` (A1, moved here from Task 19; review R1):
  - it assigns by construction (`dest`) or by merge (`odin3_net_merge`), with the same-net no-op;
  - a net declaration assignment (`wire w = e;`, N7) goes through it at the declaration;
  - a non-constant select target is EL-28's error, and out-of-range lhs bits are dropped with the warning;
  - Task 19 adds the driver-table claim before each attach and merge.
- `typedef struct odin3_elab2_cell_req { odin3_elab2_type type; odin3_ast_id node; const odin3_sig *a, *b; uint32_t y_width; const odin3_net_id *dest; } odin3_elab2_cell_req;` (the type is Task 17's enum)
- `odin3_status odin3_elab2_cell(odin3_elab2 *e2, const odin3_elab2_cell_req *req, odin3_sig *y);` follows genrtlil's parameter rules: operand widths as generated, `*_SIGNED` = signed in context, the node's attributes, its provenance. The cell-pins cap of point (aa) applies.
- `odin3_status odin3_elab2_to_nets(odin3_elab2 *e2, const odin3_sig *sig, odin3_net_id *nets);`
- `odin3_status odin3_elab2_extend(odin3_elab2 *e2, const odin3_sig *in, uint32_t width, bool is_signed, odin3_sig *out);` is wiring only, never a cell.
- `odin3_status odin3_elab2_select(odin3_elab2 *e2, const odin3_elab2_expr_req *req, odin3_sig *out);` handles constant selects, out-of-range x with the warning, and non-constant reads through `$shiftx` with genrtlil's offset computation.

**Tests (`test_elab_stage2_expr.c`; every cell's type and every parameter asserted). Signals:** 8-bit `a`, `b`, `y`; `signed [7:0] s`; `signed [3:0] t`; `[3:0] u`; 64-bit `y64`.
- **Assign by construction.** `y = a + b` → one `$add`: `A_WIDTH` 8, `B_WIDTH` 8, `Y_WIDTH` 8, `A_SIGNED` 0, `B_SIGNED` 0, its `Y` pins on `y`'s nets, no merge.
- **Assignments (review R1).**
  - `assign b2 = a;` → one net with alias count 1.
  - `assign c = 1'b1;` → `c` merged onto the `$_CONST1_` net.
  - `wire w = a & b;` (N7) → one `$and` driving `w`.
  - `assign y[s] = d;` → `continuous assignment to 'y' with a non-constant index is not supported`.
  - `assign y[9:6] = d4;` → `warning: assignment to bits outside 'y' [7:0] is ignored`.
- **E22 (Review Focus 2).**
  - `y = (a + b) >> 1` → `$add` `Y_WIDTH` 8, then `$shr` with `A_WIDTH` 8, `B_WIDTH` 32 on the constant nets for 1, `Y_WIDTH` 8.
  - `y = (0 + a + b) >> 1` → two `$add`s with `Y_WIDTH` 32, then `$shr` `Y_WIDTH` 32, truncated to `y` by wiring.
- **Unsized constants (Review Focus 2; review I11).** `localparam P = 'hFFFFFFFF; localparam [31:0] Q = 'hFFFFFFFF; localparam [35:0] R = 'hFFFFFFFF;`, each assigned to a 64-bit output:
  - `P` and `Q` → all 64 bits on the constant-1 net;
  - `R` → 32 ones, then 32 zeros.
- **E23.** `s + u` → `$add` with `A_SIGNED` 0, `B_SIGNED` 0, `A_WIDTH` 8, `B_WIDTH` 4. `s + t` → both `SIGNED` 1.
- **Every EL-30 row** (review M9; widths per ruling I7):
  - `a - b` → `$sub`; `a * b` → `$mul`; `a / b` → `$div`; `a % b` → `$mod`.
  - `-a` → `$neg` with `A_WIDTH` = the context width (extended by wiring first). `~a` → `$not`. `+a` → no cell.
  - `y = &a` → `$reduce_and` `Y_WIDTH` 8. `{&a, b}` → `$reduce_and` `Y_WIDTH` 1.
  - `y = ~&a` → `$reduce_and` (`Y_WIDTH` 1), then `$logic_not` (`A_WIDTH` 1, `Y_WIDTH` 8) (v4).
  - `|a` → `$reduce_or`; `y = ~|a` → `$reduce_or` + `$logic_not`; `^a` → `$reduce_xor`; `~^a` → `$reduce_xnor`.
  - `y = !a` → `$logic_not` `Y_WIDTH` 8. `y = a && b` → `$logic_and` `Y_WIDTH` 8. `a || b` → `$logic_or`.
  - `a ~^ b` → `$xnor`.
  - `a << b` and `a <<< b` → `$shl`; `a >> b` → `$shr`; `s >>> b` → `$sshr`; `B_SIGNED` 0 throughout.
  - `a ** b` → `$pow` with `B_SIGNED` 0. `2 ** b` → `$shl` with A = constant 1.
  - `y = a < t` → `$lt` with both operands generated at 8 bits and `Y_WIDTH` 8.
  - `c ? a : b` (1-bit `c`) → `$mux` A=`b`, B=`a`, S=`c`, `WIDTH` 8. With a 4-bit `c` → `$reduce_bool` then `$mux`. `1 ? a : b` → no cell, `y` merged with `a`.
  - `{a, b}`, `$signed(a)` and `$unsigned(s)` → no cell.
  - `y64 = {2{a}}` → no cell: the upper 48 bits on the constant-0 net (zero extension).
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
- **Hand-built store (review I10).** The `y = (a + b) >> 1` module built with `eb_*` and run through `odin3_ast_elaborate` gives the same cells as the stage-1 path.
- **Caps (point (aa)).** The cell-pins hook at 16 with a 9-bit `+` → `cell of 27 pins exceeds the limit of 16 pins`, fatal.
- **Depth.** A 2,000-term `&` chain → 1,999 `$and`s, no recursion.
- OOM sweep.

### Task 19: Instances, gates, continuous assignments, drivers, wired nets, tri-state (EL-10 widths, EL-11, EL-26, EL-28, EL-29)
**Files:** Create `src/frontends/elab/elab2_inst.c`, `elab2_drive.c` and `tests/unit/test_elab_stage2_drive.c`; modify `elab2_assign.c` (the claim before each attach), `elab2.c` (steps 2, 4, 7) and `elab_pending.c` (`INSTANTIATION`/`INSTANCE`/`CONNECTION`/`GATE_DECL` landed in stage 2; review R1).

**Consumes:** Tasks 17–18; `odin3_net_merge`; `odin3_celltype_infer_params`; `odin3_celltype_declare_blackbox`; `odin3_node_create_connected`.

**Produces:**
- `typedef enum odin3_drv_kind { ODIN3_DRV_NONE = 0, ODIN3_DRV_PORT_IN, ODIN3_DRV_CONST, ODIN3_DRV_CELL, ODIN3_DRV_INSTANCE, ODIN3_DRV_GATE, ODIN3_DRV_ASSIGN, ODIN3_DRV_PROCESS, ODIN3_DRV_INOUT_PIN, ODIN3_DRV_TRIBUF } odin3_drv_kind;`
- `typedef struct odin3_drv { uint8_t kind; odin3_ast_id node; } odin3_drv;` (the table is a `vec` indexed by net ID)
- `typedef struct odin3_elab2_claim_req { const odin3_net_id *nets; uint32_t n; uint8_t kind; odin3_ast_id node; } odin3_elab2_claim_req;`
- `odin3_status odin3_elab2_claim(odin3_elab2 *e2, const odin3_elab2_claim_req *req, bool *granted);` checks every bit **before** attaching. It reports A4, `uwire`, `supply`, wired-net collection, the tri-state and `inout` rules, and leaves conflicting bits unattached.
- `odin3_status odin3_elab2_prescan(odin3_elab2 *e2, odin3_ast_id module_node);` is step (2)'s always-assigned bit set (also used by EL-27) and the wired-net procedural check.
- Task 18's `odin3_elab2_assign` now claims every target bit before it attaches or merges, which gives A4, the variable-assigned warning and the self-loop warning.
- `supply0`/`supply1` nets are driven by the shared constant nets.
- Once per module, a wire with sinks and no driver warns `'w' is never driven`.
- `odin3_status odin3_elab2_instance(odin3_elab2 *e2, odin3_ast_id instance);` (I4 resizing, unconnected inputs to `$_CONSTX_`, `parameter:<NAME>` attributes, cell types with inferred parameters, black boxes declared per specialization)
- `odin3_status odin3_elab2_gate(odin3_elab2 *e2, odin3_ast_id gate_instance);` lowers gates as Yosys's `simplify` does:
  - `and`/`or`/`xor` → a left fold of 1-bit word cells;
  - `nand`/`nor`/`xnor` → the fold, then `$not`;
  - `buf`/`not` → one assignment per output;
  - `bufif*`/`notif*` → a ternary with z, i.e. `$tribuf`.
- `odin3_status odin3_elab2_wired(odin3_elab2 *e2, odin3_ast_id module_node);` is step (7): `$and`/`$or` chains in source order, each with the net's declarator as provenance.

**Tests (`test_elab_stage2_drive.c`):**
- **Assignments with drivers.**
  - `assign p = q; assign q = p;` → no error, plus `warning: 'p' and 'q' are connected only to each other and are never driven`.
  - `wire w; assign y = w;` → `warning: 'w' is never driven`, once.
- **A4.**
  - `assign y = a; assign y = b;` → `t.v:4:10: error: 'y' has two drivers (also driven at t.v:3:10)`.
  - Whole then `assign y[3] = c;` → `'y[3]' has two drivers (also driven at t.v:3:10)`.
  - IR check passes in both cases.
- **Variables and special nets.**
  - `reg r; assign r = a;` → `warning: 'r' is a variable assigned by a continuous assignment; treated as a net`.
  - A `uwire u` with two `assign`s → `'u' is a uwire and has a second driver (first at t.v:3:10)`.
  - `supply0 s; assign s = a;` → `'s' has two drivers (also driven at t.v:2:9)`.
  - `supply1 v;` → `v`'s net is the `$_CONST1_` net.
- **Wired nets.**
  - `wand w` with three whole-net drivers → two `$and` cells of the net's width, in source order, each with a SOURCE record whose `ast` is `w`'s declarator (review M10).
  - `wor` with one driver of `w[3:0]` and one of `w[7:0]` → per-bit `$or`s.
  - One driver → no cell.
  - `always @* w = a;` on a `wand` → `'w' is a wired net (wand) and cannot be assigned in an always block` (from the pre-scan).
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
  - `single_port_ram` (with `pre_script` `read_techlib lib/vtr.o3lib`) and 10/8-bit address/data connections → `ADDR_WIDTH` 10, `DATA_WIDTH` 8 by inference.
  - A `(* blackbox *)` module → `odin3_celltype_declare_blackbox` **once per definition, under its base name** (spec v4.10, EL-11; PR 2D-3 final review I2, ruling: Yosys 0.55 keeps `module \bb` and `cell \bb \u1` with `parameter \W 8`). Stage 1 writes one ports-only `MODULE` named `bb` (the defaults' ports and values) and every `INSTANTIATION` of the definition is named `bb` with its final values in its params LIST; it never writes `bb$W=8`. Task 19 declares the type from that `MODULE`; each cell takes its `INSTANTIATION`'s values as cell parameters, and its port widths are its own specialization's (stage 1 has checked the connections against them): the type is parametric (IR-7b width rules, for a port whose width depends on a parameter) or Task 19 records the widths per cell from stage 1's specialization table — Task 19 decides, and a black box whose port widths do not depend on its parameters needs neither. `write_blif --vtr` (3E) writes `.subckt bb`, never a `$`-suffixed name, so an architecture model named after the definition matches. A mismatch with a registered type → located `black box 'single_port_ram' does not match the registered cell type: <IR's why>`; a source `(* blackbox *)` named like a registered type reuses it whatever its parameters (IR-7b's check by port name).
- **Error cap and top (moved from Task 17; review R1).**
  - 25 two-driver assignments → 20 errors, then `too many errors; elaboration stopped after 20`, `PARSE`, `odin3_design_top` unset, IR check passes.
  - A clean three-module design (one specialization) → `odin3_design_top` set, and the summary line `… 3 modules (1 specializations), …`.
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
- `typedef struct odin3_elab2_mem_wr { uint32_t mem; const odin3_sig *addr, *data, *en; odin3_net_id clk; bool posedge; odin3_ast_id stmt; } odin3_elab2_mem_wr;`: `addr`/`data` may be constant sigs (v4: no temporary for a constant index or full-word value).
- `odin3_status odin3_elab2_mem_write(odin3_elab2 *e2, const odin3_elab2_mem_wr *wr);` (used from Task 24)
- `odin3_status odin3_elab2_mem_emit(odin3_elab2 *e2);` is step (8): one `$mem` per memory, ports in source order.
- Register-per-word arrays become wires `m[k]` (declared index). A constant-index access reads the word; out of range reads x with the warning. A variable-index read outside a process is the pending feature `ODIN3_PEND_REGS_VAR_READ` until Task 23.

**Tests (`test_elab_stage2_mem.c`):**
- **mem2reg rules.**
  - Rule 1: `wire [3:0] w [0:3]` → wires `w[0]`…`w[3]`, no warning. `(* mem2reg *)` on the array and on the module → registers.
  - Rule 2: written in `always @*` → registers plus `warning: replacing memory 'm' with list of registers; see t.v:12`. Written in `always @(posedge c or posedge r)` → registers.
  - Rule 3: read after a blocking write in the same block → registers.
  - Rule 4: `m[m[0]] <= x` and `{m[0], a} <= v` → registers.
  - Rule 5: constant-index writes only → registers.
  - `(* nomem2reg *)` exempts rules 2–5.
  - `--nomem2reg` with a net array assigned by `assign` → `array 'w' cannot be a memory (it is written in a combinational block) and --nomem2reg is set`.

  (Rules 2–5 are decided from the pre-scan, so they are testable before processes exist. The blocks themselves stay pending until Task 21.)
- **Addressing (Review Focus 4)**, for a read-only array (no writes outside `initial`, so not rule 5):
  - `reg [7:0] m [1:4]; assign q = m[ra];` → one `$mem` with `MEMID` `"m"`, `WIDTH` 8, `SIZE` 4, `OFFSET` 1, `ABITS` = width(`ra`), `RD_PORTS` 1, `WR_PORTS` 0, `RD_CLK_ENABLE` `1'b0`, `RD_TRANSPARENT` `1'b0`, `RD_EN` on the constant-1 net, `RD_CLK` on the x net, and `RD_ADDR` pins **on `ra`'s nets** (raw index, no `$sub`).
  - `[4:1]` → `OFFSET` 1. `[-2:1]` → `OFFSET` −2.
  - `reg [7:0] m2 [0:3][1:2]` read `m2[i][j]` → `OFFSET` 0, `SIZE` 8, the address `(i − 0) × 2 + (j − 1)` as `$sub`/`$mul`/`$add` cells.
- **Registers per word.**
  - `wire [3:0] w [0:3]; assign w[1] = a; assign y = w[1];` → `y` merged with `a`.
  - `w[7]` → x plus the out-of-range warning.
  - `assign y = w[s];` → `error: elaboration is not available yet (2D): 'variable index into a register array'` (Task 23 flips it).
- **Whole-array read.** `assign y = m;` → `'m' is a memory; select a word`.
- **Cap.** The memory-bits hook at 1024 with a 2048-bit array → `memory 'm' of 2048 bits exceeds the limit of 1024 bits`.
- OOM sweep.

## PR 2D-6 — stage 2: processes

### Task 21: The process generator core and the Yosys bridge (EL-33 emission, EL-34, EL-35, §14.3; ruling C2)
- **Interface owed to 2E's `proc-diff` (2E-4a final review, 2026-10-12):** `tools/elab-check/memcap.py` provides `memcap.run(argv: list[str], cap_bytes: int, capture_output: bool = False) -> CompletedProcess`; with `capture_output=True` the child's stdout/stderr are returned (2E needs them to recognise §20 #16/#17). The cap is in bytes.
**Files:**
- Create `src/frontends/elab/pg.h`, `pg.c` and `tests/unit/test_elab_stage2_proc.c`.
- Create the bridge files (ruling C2): `tools/elab-check/elab-equiv` (shell wrapper), `tools/elab-check/elab_equiv.py`, `tools/elab-check/memcap.py` (point (n)) and `tests/tools/test_elab_equiv.py`.
- Modify `elab2.c` (step 6) and `elab_pending.c` (`ALWAYS`, blocks and assignments landed in stage 2).
- Modify `CMakeLists.txt`: the Python test in `python_tools`; CTest `elab_proc_bridge_smoke` inside Task 5's 0.55 block.

**Consumes:** Task 3 (`odin3_proc_body`, `odin3_proc_body_pack`, `$proc`, the canonical layout), Tasks 17–19 (`odin3_elab2_expr` with `reads`, `odin3_elab2_claim`, `odin3_elab2_const_net`, `odin3_elab2_prov`, `odin3_sigbit`), the binding.

**Produces (`pg.h`):**
- Lifetime and entry:
  - `typedef struct odin3_pg odin3_pg;` holds the builder vectors for cases, switches, actions, compares and syncs (bits are `odin3_sigbit`, review M21), the `lv`/`rv` maps with an undo log, the per-bit action-slot index for removals, and the frame stack.
  - `odin3_status odin3_pg_create(odin3_elab2 *e2, odin3_pg **out);` and `void odin3_pg_destroy(odin3_pg *pg);`
  - `odin3_status odin3_pg_always(odin3_pg *pg, odin3_ast_id always_node);` builds one block and creates one node, or none (§8.3 step 7).
- Statement and assignment machinery:
  - `odin3_status odin3_pg_stmt(odin3_pg *pg, odin3_ast_id stmt);` dispatches a statement through an explicit frame stack.
  - `typedef struct odin3_pg_assign_req { odin3_ast_id stmt, lhs; const odin3_sig *rhs; bool blocking; } odin3_pg_assign_req;`
  - `odin3_status odin3_pg_assign(odin3_pg *pg, const odin3_pg_assign_req *req);` (step 5)
  - **Chunks** (spec v4.2, EL-35 steps 1, 3–5; amended with the 2E/2F plans v2, ruling I7).
    - Root defaults (step 3) and sync actions (step 4) follow the **sorted** outputs, one action per output chunk. A chunk is a maximal run of consecutive bits of one variable.
    - User assignments (step 5) are split in **lvalue order** into runs of consecutive bits of one temporary group. Yosys's `addChunkActions` does not sort.
    - Temporaries are grouped by the output chunks.
    - Cost: one O(outputs) grouping per block.
  - `uint32_t odin3_pg_save(const odin3_pg *pg);` and `void odin3_pg_restore(odin3_pg *pg, uint32_t mark);` (the undo log)
  - `odin3_status odin3_pg_temp(odin3_pg *pg, uint32_t width, odin3_ast_id why, odin3_net_id *first);` makes fresh process-local nets.
  - `odin3_status odin3_pg_emit(odin3_pg *pg, odin3_ast_id prov_node, odin3_node_id *out);` runs step 7:
    - renumber cases, switches and actions breadth-first into point (e)'s canonical layout (the builder may allocate);
    - `O` pins and then `I` pins in net-ID order, constants as `I` pins on the shared constant nets, ordered by net ID among the other `I` pins (spec EL-35 step 7, v4.3; 2E's decode→encode identity relies on it);
    - the driver-table claim for every `O` net, dropping the actions of conflicting bits;
    - one `odin3_node_create_connected`, whose type is `odin3_elab2_type_id(e2, ODIN3_E2T_PROC)` from the stage-2 cache (review R6);
    - the `always`'s attributes on the node (`ODIN3_OBJ_NODE`).
- Syncs (EL-34): one per edge event, or one `ALWAYS`. Errors:
  - `posedge on 'x', which is N bits; a 1-bit signal is needed` (and `negedge`);
  - `edge and level events in one event list are not supported`, for adapter stores.
- The T3 analysis names the first missing signal by name order.

**Produces (bridge, §14.3; review I9):** `elab-equiv --odin3 O --yosys Y --equiv-check E [--top T] [--mem-cap B] [--keep DIR] [--allow-version V] [--a-only] <file>`:
- **A** = `O [--top T] -p "read_verilog --nolatches F; write_rtlil a.il"`, then `Y -p "read_rtlil a.il; hierarchy -top T; proc; flatten; opt; memory -nomap; opt; memory_map; opt -full; techmap; opt -fast; abc -g AND,NAND,OR,NOR,XOR,XNOR,MUX; opt_clean; write_blif -gates -undef + unconn a.blif"`. Without `--top`, `hierarchy -auto-top`.
- **B** = the same Yosys script with `read_verilog -sv -nolatches F` in place of `read_rtlil a.il`. Both sides are always `--nolatches` (`equiv-check` cannot model latches, PHASE0 #7).
- `Y -p "read_rtlil a.il; write_rtlil b.il"` must exit 0.
- Then `E a.blif b.blif`.
- Exit 0 equivalent, 1 not, 2 error.
- `--a-only` stops after `a.blif` (kept in `--keep DIR`), the hand-off to 2H (point (m)).
- Every child runs through `memcap.run(argv, cap)` (point (n)).
- It refuses a Yosys whose `-V` lacks `0.55` unless `--allow-version` names it (the nightly drift job only).

**Tests:**

`test_elab_stage2_proc.c`, each `$proc` body asserted exactly (`I`/`O` pin nets in order, every case, action and sync; canonical layout):
- **T7 (Review Focus 3).** `module t(input clk, a, b, output reg r); reg t; always @(posedge clk) begin t = a & b; r <= t; end endmodule` gives:
  - `I` = [`clk`, the `$and`'s Y];
  - `O` = [`r`, `t`, t0(`r`), t0(`t`)];
  - root actions `O3 ← I1`, `O2 ← I1`, in that order;
  - one `POSEDGE I0` sync with `O0 ← O2`, `O1 ← O3`.
- **Chunks (v4.2).**
  - `reg a, b; always @(posedge c) {a, b} <= {x, y};` → two sync actions (`a ← t0(a)`, `b ← t0(b)`) and two root actions, in sorted output order, and two `t0` groups, one per variable. The user assignment is split in lvalue order: `t0(a) ← x` first, then `t0(b) ← y`.
  - `always @(posedge c) {b, a} <= {x, y};` (a declared before b) → the root and sync actions in sorted order (`a` first), and the user assignment's actions in lvalue order (`t0(b) ← x`, then `t0(a) ← y`).
  - `reg [7:0] q; always @(posedge c) q <= d8;` → one 8-bit sync action and one 8-bit root action, not eight 1-bit ones.
  - `always @(posedge c) q[3:0] <= d4;` on 8-bit `q` → actions on the chunk `q[3:0]` only; the write splits at the chunk boundary.
  - A constant `I` pin (`r <= 1'b0` beside `a`) sits at its net-ID position among the `I` pins.

  That is two outputs (§14.1). The `$and` has A=`a`, B=`b`.
- **T8.** `r1 <= a; r2 <= r1;` gives `I` = [`clk`, `a`], `O` = [`r1`, `r2`, t0(`r1`), t0(`r2`)], root `O2 ← I1`, `O3 ← O0` (the held value, §8.1 N6), and the sync updating both.
- **Held value.** `q <= q + 1` → the `$add` reads `q`'s net; root `O1 ← I1`; the sync `O0 ← O1`.
- **Combinational default mode.** `always @* y = a;` → root `t0(y) ← a`, one `ALWAYS` sync `y ← t0(y)`.
- **Constants.** `r <= 1'b0` → `I` holds the shared `$_CONST0_` net once; a second `<= 1'b0` in the same block reuses that pin.
- **Event lists.**
  - `@(posedge a or negedge b)` → syncs `POSEDGE I(a)`, `NEGEDGE I(b)` in order.
  - `@(a or b)`, `@(a, b)`, `@a`, `@*` → one `ALWAYS`.
  - `@(posedge w4)` → `posedge on 'w4', which is 4 bits; a 1-bit signal is needed`.
  - `always @(a) y = a & b;` → `warning: incomplete sensitivity list: 'b' is read but not listed; the block is built as @*`, once.
- **Point (h).** `always @(posedge a) a <= ~a;` → a `POSEDGE` sync whose signal is `OUT|0`, accepted.
- **Net in a process.** `wire w; always @* w = a;` → `warning: 'w' is a net assigned in an always block; treated as a variable`.
- **Attributes and provenance (review M10).** `(* keep *) always @* y = a;` → the node has attribute `keep`. The node's SOURCE record is its `ALWAYS`; each action, case and switch records its statement (EL-45).
- **No node.** `always @* ;` → no node, `odin3_module_proc_count` 0.
- **Different bits.** Two blocks writing `r[0]` and `r[1]` → two nodes, no error.
- **Pending.** An `if` inside an `always` (until Task 22) → `elaboration is not available yet (2D): 'IF'`.
- **Hand-built store (review I10).** The T7 module built with `eb_*` and run through `odin3_ast_elaborate` gives the same body.
- **Determinism and round trip.** Two runs give byte-identical `write_rtlil`. `odin3_check_design` FULL passes on every result. `write_rtlil` → Yosys 0.55 `read_rtlil; proc` exits 0.
- OOM sweep over `odin3_pg_always`: `NO_MEMORY`, no node created, no leak.

`test_elab_equiv.py`, over fake `odin3`, `yosys` and `equiv-check` scripts that log their argv:
- the A argv with and without `--top T` (`odin3 --top T -p "read_verilog --nolatches F; write_rtlil a.il"`);
- the two Yosys command strings exactly, `-nolatches` on B always;
- the exit-code mapping;
- `--a-only` writes `a.blif` and never runs B or `equiv-check`;
- a `0.69` version refused, and accepted with `--allow-version 0.69`;
- `memcap.run` wraps each child in `systemd-run --user --scope -q -p MemoryMax=… -p MemorySwapMax=0` when available;
  (2E-1 final review: a Python driver that spawns nested heavy steps must pass `pass_fds=(9,)`, or a nested `tools/heavy-run` exits 2 and `build-locked` refuses;)
- an ASan child without `systemd-run` → exit 77;
- `memcap.run` refuses a cap above 60% of `MemTotal` (a fake `/proc/meminfo`).

CTest `elab_proc_bridge_smoke`: `elab-equiv` on the T7, T8 and `q + 1` sources → exit 0.

### Task 22: `if`, `case`, case temporaries, `full_case`/`parallel_case`, non-constant lvalue selects (EL-36, EL-32 writes)
**Files:**
- Create `src/frontends/elab/pg_case.c` and `tests/unit/test_elab_stage2_case.c`; modify `elab_pending.c` (`IF`, `CASE` landed in stage 2).
- Modify `CMakeLists.txt` (CTest `elab_case_bridge` in the 0.55 block).
- Modify `docs/specs/2026-10-09-2A-ast-design.md` §7: the D1 warning is emitted by 2D, not 2E (EL-36, §19).

**Consumes:** Task 21.

**Produces:**
- `odin3_status odin3_pg_if(odin3_pg *pg, odin3_ast_id stmt);` and `odin3_status odin3_pg_case(odin3_pg *pg, odin3_ast_id stmt);` implement EL-36:
  - case temporaries t1 for bits the branches assign by blocking assignment, with `lv`/`rv` saved and restored around each case. Per v4.2 (EL-36; ruling I7), t1 is grouped by chunks, and every entry, default and exit action is one action per chunk;
  - the added default case (`t1 ← rv(bits)`, or x under `full_case`);
  - `casez`/`casex` `ANY` positions;
  - non-constant item expressions (C8) generated as compare signals;
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
- **C8 (review M10).** `case (1'b1) a: y = 1; b: y = 2; endcase` → compares on the `I` pins of `a` and `b` (non-constant items).
- **Wildcards.** `casez` `2'b1?` → compare [`1`, `ANY`]. `casex` `2'bx1` → [`ANY`, `1`]. Plain `case` `2'bx1` → [x-net pin, `1`].
- **Case width.** `case (a4) 3'd1:` → the compare is 4 bits.
- **Flags.**
  - `(* full_case *) case (s) 1'b0: y = a; endcase` → switch flag `FULL_CASE`, an added default with [`t1 ← 1'x`], and `warning: 'full_case' is honoured: case values no item lists assign x`.
  - `// synopsys full_case parallel_case` → both flags and both warnings, the second `warning: 'parallel_case' is honoured: case items are not prioritised`.
- **Constants.** `if (1) y = a; else y = b;` → no switch. `case (2'd1)` with constant items → folded to that item.
- **Non-blocking in a branch.** `always @(posedge c) if (e) q <= d;` → root [`t0 ← q`], switch `e`, case [`1'1`] with [`t0 ← d`], and no t1 net.
- **Nesting (internal risk §16 #3).** `if (a) begin y = 1; if (b) y = 2; end` → nested switches, each with its own t1, and outer state restored after the inner one.
- **Variable-index write.** `always @(posedge c) y[s] <= d;` (`y` `[3:0]`, `s` 2 bits) → position temporary `p ← s`; a switch on `p` with cases `2'00`…`2'11`, each [`t0(y)[k] ← d`]; no default.
- **Indexed part-select write.** `y[b +: 2] <= d2` (`b` 2 bits) → cases for positions 0–2 only.
- **Chunks (v4.2).** `reg [7:0] y; always @* case (s) 2'd0: y = a8; 2'd1: y = b8; default: y = c8; endcase` → each case's `t1 ← …` is one 8-bit action, and so are the added default and the exit. That this gives one 8-bit `$pmux` after `proc`, not eight 1-bit ones, is checked by 2E: its chunk check (point (g)) and the `case_default` row.
- **Ascending target.** An ascending `[0:3]` target maps positions by EL-23.
- **Bridge.** CTest `elab_case_bridge`: each test source passes `elab-equiv`.
- OOM sweep.

### Task 23: Loops, functions and tasks, continuous-context calls, mem2reg variable reads (EL-37, EL-38, EL-27 reads)
**Files:** Create `src/frontends/elab/pg_loop.c`, `pg_call.c` and `tests/unit/test_elab_stage2_call.c`; modify `elab2_expr.c` (continuous-context calls), `elab2_mem.c` (the register-per-word variable read) and `elab_pending.c` (`FOR`, `REPEAT`, `TASK_CALL`, process calls and the two read features landed).

**Consumes:** Tasks 9 (`odin3_ceval_try`, `odin3_ceval_call`), 20, 21 and 22.

**Produces:**
- `odin3_status odin3_pg_for(odin3_pg *pg, odin3_ast_id stmt);` and `odin3_status odin3_pg_repeat(odin3_pg *pg, odin3_ast_id stmt);` unroll with constant conditions, the loop frame, the final value, both caps, and point (k)'s starting count.
- `odin3_status odin3_pg_call(odin3_pg *pg, odin3_ast_id call, odin3_sig *result);` handles functions and task enables, including K10's enable without parentheses. It tries the interpreter first, then inlines through a call frame of fresh temporaries, copies outputs, and stops at point (aa)'s inline-depth text.
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
- **Point (k) (review M15).** A 12-iteration generate loop plus a 24-iteration procedural loop with the total hook at 32 → the same total error at the procedural loop. The two loops alone each pass.
- **`repeat` and shared counters.** `repeat (3)` → the body three times. A shared `integer i` looping in two blocks → no T10.
- **Functions.**
  - `function [3:0] inc(input [3:0] v); inc = v + 1; endfunction` with `q <= inc(d);` → one `$add` reading the frame temporary of `v`; the temporaries are `O` pins with no sync action; no flip-flop for `v` or `inc`.
  - `inc(4'd3)` → folded to `4'd4`, no cell.
- **Tasks.**
  - `task tk(input x, output z); z = ~x; endtask always @* tk(p, y);` → `y` gets `$not(p)`.
  - K10: `task clr; y = 0; endtask always @* clr;` → `y ← 0` (review M10).
  - `tk(p, a & b)` → `output argument 2 of task 'tk' must be a variable`.
  - Nested calls (K6, K11) and a call inside a loop → correct frames, equivalent through the bridge.
- **Continuous context.** `assign y = inc(d);` → one extra `$proc` with one `ALWAYS` sync, and `y`'s net driven by its `O` pin.
- **Inlined provenance.** Two call sites of `inc` give two distinct SOURCE records for their `$add`s, each with locs [the body's `BINARY`, the `CALL`] (EL-45).
- **Call errors (point (aa)).**
  - Recursion with non-constant arguments → `recursive call of 'f' cannot be inlined (its arguments are not constant)`.
  - Non-recursive nesting past the hook at 4 → `function call nesting deeper than 4 at 'f'`.
  - `while` with non-constant arguments → `function 'f' can be called only with constant arguments (it uses 'while')`.
  - `inc(a, b)` → `function 'inc' has 1 inputs; 2 arguments given`.
- **mem2reg variable reads.**
  - `wire [3:0] w [0:3]; assign y = w[s];` → a `$proc` with a switch on `s`, cases `2'00`…`2'11` each [`t ← w[k]`], default [`t ← 4'x`]. This flips Task 20's pending assertion.
  - Inside a block, `m[s] <= d` on a register array → a case over `s` assigning one word each.
- OOM sweep.

### Task 24: Memory writes, initial values and `$readmem`, per-bit rules (EL-39 v4, EL-40, EL-41)
**Files:**
- Create `src/frontends/elab/pg_init.c`, `tests/unit/test_elab_stage2_init.c`, `tests/unit/data/elab/readmem/{init.hex,init.bin,bad_addr.hex,bad_word.hex}` and `tests/elab/sink/sink.v` (a module using every construct of Tasks 17–24).
- Modify `pg.c` (memory write temporaries), `elab2_mem.c` (write ports, `INIT`), `elab2.c` (step 3) and `elab_pending.c` (`INITIAL` and `ODIN3_PEND_MEM_WRITE` landed; both tables now empty).
- Modify `CMakeLists.txt`: CTests `elab_mem_bridge` (0.55 block) and `elab_no_pending`.

**Consumes:** Task 9 (`odin3_ceval_exec` with an `odin3_ceval_store` over the module's variable bits), Tasks 20–23.

**Produces:**
- `odin3_status odin3_pg_mem_write(odin3_pg *pg, const odin3_pg_assign_req *req);` (EL-39 **v4**, ruling C1):
  - Up to three process-local temporaries per write statement: `addr` (the wider of the index and `ABITS`), `data` (the memory's width) and `en`.
  - The statements `addr = x`, `data = x`, `en = 0` are inserted at the block's start, in that order, on the `$_CONSTX_`/`$_CONST0_` pins. They are processed as blocking assignments, so EL-35/EL-36's removal rule shapes the final body (spec v4 EL-39; review R2).
  - No `addr` temporary for a constant index, and no `data` temporary for a constant full-word value: the constant goes to the port.
  - One write port per statement, in source order.
- `odin3_status odin3_elab2_initials(odin3_elab2 *e2, odin3_ast_id module_node);` (EL-40) produces the three result kinds, the two D2 warnings and the ignored-value warning. The init-bits cap uses point (aa)'s text.
- `odin3_status odin3_elab2_readmem(odin3_elab2 *e2, odin3_ast_id call, const odin3_ceval_store *store);` parses as Yosys's `readmem` does, with point (y)'s path rule and point (aa)'s size text.
- The per-bit T9/T10 check at claim time, both places named from the symbol table's assignment lists.

**Tests (`test_elab_stage2_init.c`):**
- **Write ports (Review Focus 4; ruling C1).** `reg [7:0] m [1:4]; wire [2:0] wa, ra; always @(posedge c) if (we) m[wa] <= d; assign q = m[ra];` gives one `$mem` with:
  - `WR_PORTS` 1, `WR_CLK_ENABLE` `1'b1`, `WR_CLK_POLARITY` `1'b1`, `WR_CLK` = `c`;
  - `WR_ADDR`/`WR_DATA`/`WR_EN` = the `$proc`'s three `O`-pin temporaries, which have no sync action;
  - the body has a root with actions [`addr ← t1a`, `data ← t1d`, `en ← t1e`] in that order, where t1a, t1d and t1e are the switch's case temporaries, and one switch on `we`;
  - that switch has case `1'1` with [`t1a ← wa`, `t1d ← d`, `t1e ← 8'hFF`], and the added default case with [`t1a ← 3'xxx`, `t1d ← 8'xxxxxxxx`, `t1e ← 8'h00`] (the x/x/0 defaults, review R2);
  - every one of the three temporaries is the `lhs` of a root action, so no path leaves them unassigned;
  - `RD_ADDR` = `ra` raw, `OFFSET` 1.
- **Constant index and value.** `m[2] <= 8'h5A;` → no `addr` or `data` temporary. `WR_ADDR` and `WR_DATA` are pins on the constant nets for `3'b010` and `8'h5A`. The root holds only [`en ← 8'hFF`]: the unconditional write's action removed the inserted `en ← 0` (review R2).
- **Other bases.** `[4:1]` → `OFFSET` 1. `[-2:1]` → `OFFSET` −2.
- **Bridge.** CTest `elab_mem_bridge` runs `elab-equiv` on all three ranges, each with a write then a read at the same index and with a cycle where `we` is 0 → equivalent (no feedback loop on side A).
- **Partial and multiple writes.** `m[i][3:0] <= d4` → `en` `8'h0F`. Two write statements → two write ports in source order.
- **Initial values.**
  - `reg [3:0] r = 4'd5; always @(posedge c) r <= r + 1;` → wire `r` has `init` = `4'b0101`.
  - `initial begin m[1] = 8'h12; for (i = 2; i <= 4; i = i + 1) m[i] = i; end` → `$mem` `INIT` = words 1..4 = `12 02 03 04`.
  - `initial r2 = a;` (`a` an input) → `warning: initial statement is not constant and is ignored`.
  - `reg k; initial k = 1;` with nothing else driving `k` → `k`'s net merged onto `$_CONST1_`.
  - `assign w = a; initial w = 0;` → `warning: initial value of 'w' is ignored: 'w' is driven by a continuous assignment`.
  - The same for an instance output → `… driven by an instance` (review M10); for an input port → `… driven by an input port`.
  - The init-bits hook at 16 with a 32-bit initialised memory → `t.v:4:5: error: initial values need more than 16 bits`.
- **`$readmem`.**
  - `$readmemh("init.hex", m)`, the test running from another cwd, the file holding `// c`, `/* x */`, `@2` and `12 34` → `INIT` words 2, 3 = `12`, `34`, the rest x.
  - `$readmemb("init.bin", m, 1, 2)` → words 1–2.
  - `"nope.hex"` → `cannot open 'nope.hex' for $readmemh`.
  - `bad_addr.hex` → `bad address '@zz' in 'bad_addr.hex' line 3`.
  - `bad_word.hex` → `bad word 'g1' in 'bad_word.hex' line 1`.
  - `$readmemh` in an `always` → `$readmemh is supported only in an initial block`.
  - The readmem hook at 16 bytes → `'init.hex' is larger than 16 bytes (the $readmem limit)`.
- **Per-bit rules.**
  - T9: `r = a; r <= b;` in one block → `t.v:4:5: error: 'r' is assigned with both '=' and '<=' (also at t.v:3:5)`; also across two blocks; an inlined task output counts as `=`.
  - T10: `always @(posedge c) r[2] <= a; always @(posedge c) r[2] <= b;` → `'r[2]' is assigned in two always blocks (also at t.v:3:22)`. `r[1]`/`r[2]` split across blocks → OK.
  - A4: `assign r = a; always @* r = b;` → `'r' has two drivers (also driven at t.v:3:10)`, and the IR has a single driver.
- **Pending gone (point (l)).**
  - `odin3_elab_pending(s, k)` is false for both stages and every kind and feature.
  - CTest `elab_no_pending`: every `tests/verilog/coverage/*.v` and `tests/elab/sink/sink.v` runs `read_verilog` without the text `elaboration is not available yet (2D)`.
  - `verilog_reject_2d` and `verilog_ignore_2d` pass with **0 pending**.
- **Sink.**
  - OOM over both stages of `sink.v`: `NO_MEMORY` at every injection point, no leak, `odin3_check_design` FULL passes on what is left.
  - Two runs give byte-identical `write_rtlil`.

## PR 2D-7 — evidence (grows from 2D-3, closes last)

### Task 25: `elab-check`, the coverage rows, both latch modes' goldens, the sweep preset (§14.2)
**Files:**
- Create `tools/elab-check/elab-check.c` (C; links `odin3_core` like `odin3-parse-check`) and `tools/elab-check/micro-top` (Python, point (o)).
- Create `tests/elab/rows/rows.txt` (review M19). One line per row: `<row> <file> comb=<0|1> clocks=<N>`, where `comb=1` means the file has a combinational `always` and `clocks` counts distinct edge signals.
- Create `tests/elab/rows/<row>_<slug>.v` for the 125 MUST and SHOULD rows, each starting `// <row>: <construct> — variants: …` and derived from 2C's coverage file minus its parse-only variants.
- Create `tests/elab/rows/<row>.elab`:
  - The 54 rows of §14.2's list (point (z)) are hand-written, first line `# hand-written`: M1, M2, M12, P1–P6, P8, N2, N7–N13, E8, E9, E12, E15, E16, E22, E23, G1–G7, C4–C9, T7, T18, F1, K1–K11, I3, I6.
  - Controller ruling (Task 25 review I2, 2026-10-13): the 54 goldens stay seeded from `--write` and are accepted as regression locks, not independent derivations. Line 2 reads `# seeded by elab-check --write; regression lock; Yosys-equivalent[; key fields hand-checked <date>]`; the 19 PARAM_DECL/GEN_BLOCK rows were hand-checked (`task-25-golden-check.md`). Full independent derivation of the stage-1 content stays owed (Task 28 / PR 2D-7 close). Peter may override.
  - Drafts written while 2D-3 is in review are provisional. Each is re-derived once against point (bb) and Task 15's tests after 2D-4 merges, and frozen in its own commit (review I8).
  - The rest are generated with `--write` and reviewed once in the diff.
- Create `tests/elab/rows/<row>.il` (default mode) and `<row>.nolatches.il` for every `comb=1` row.
- Create `tests/elab/equiv_expect_fail.txt` and `tests/tools/test_elab_rows.py`.
- Modify `CMakeLists.txt`: CTest `elab_coverage` (label `elab`, in the gate); CTest `elab_coverage_equiv` (label `sweep`, `RUN_SERIAL`, in the 0.55 block).
- Modify `CMakePresets.json`: `debug` and `release` test presets exclude labels `sweep` and `sweep-xl`, and the `sweep` and `sweep-xl` test presets are added exactly as point (n) states (no workflow preset; point (n) as amended).

**Consumes:** every earlier task; `odin3_ast_dump`; `odin3_rtlil_text`; Task 21's `elab-equiv` and `memcap.py`.

**Produces:** `elab-check [--write] [--nolatches] [--stage1] [--top T] <file>…`. For each file, run from its own directory under its basename:
1. `read_verilog` (both stages, Debug checks on), or stage 1 only with `--stage1`, which the controller uses on PR heads before 2D-5;
2. `odin3_ast_check` FULL, and `odin3_check_design` FULL unless `--stage1`;
3. compare `odin3_ast_dump(elab)` with `<row>.elab` (`--write` refuses a `# hand-written` file);
4. unless `--stage1`, compare `odin3_rtlil_text` with `<row>.il`, or with `<row>.nolatches.il` under `--nolatches`.

Exit 0 if all pass, 1 if any differs, 2 for usage or I/O errors. `elab_coverage_equiv` runs `elab-equiv` per row, smallest first.

**Tests:**
- `test_elab_rows.py`:
  - `rows.txt` lists exactly the 125 IDs whose verdict in the audit's §4 tables starts with `MUST` or `SHOULD`, plus N15's `uwire` SHOULD, each once, each line matching the format;
  - every file exists with its header;
  - exactly the 54 hand-written rows have an `.elab` starting `# hand-written`;
  - every `comb=1` row has `.nolatches.il`.
- `elab_coverage`: green on all 125 rows, smallest first, in both modes.
- `elab_coverage_equiv` (`ODIN3_HEAVY_OK=1 ctest --preset sweep`): green except the rows in `equiv_expect_fail.txt`, each with its reason. Rows with `clocks` above 1 rely on their goldens (§14.2).
- `ctest --preset debug -N` lists no test labelled `sweep`.

### Task 26: Project corpus, the handover close-out and the micro sweeps (§14.4, §14.5)
- **Interface owed to 2E Tasks 11–13 (2E-4a final review, 2026-10-12):** `elab_sweep.collect(paths: list[str]) -> …` (the corpus collector `proc-diff`'s sweeps reuse), plus a per-file top for `proc-diff` (`--tops F`: one `<file> <top>` per line) and an `xl` skip for files over the heavy cap.
**Files:**
- Create `tools/elab-check/elab-sweep` (Python): smallest first, `memcap.py`'s caps, expect-fail lists, modes `--elab-only`, `--equiv` and `--bench B` (point (t)).
- Create `tests/elab/micro_expect_fail.txt` and `tests/tools/test_elab_sweep.py`.
- Modify `tools/project-fixtures/project_fixtures.py`: a `--with-frontend` mode that runs `elab-equiv` on the positive phase-2 cases and kind/location/name checks on the negative ones.
- Modify `CMakeLists.txt`: CTests `verilog_micro_elab` (label `sweep`), `elab_equiv_micro` (label `sweep`, 0.55 block) and `project_corpus_frontend` (gate; 0.55 block).

**Consumes:** Tasks 21 and 25; 2B-3's `project-fixtures compare`; the Parmys golden tree (read-only).

**Produces:**
- `elab-sweep --odin3 O [--equiv --yosys Y --equiv-check E | --bench B] [--expect-fail F] [--jobs J] [--mem-cap C] <dir or file>…`:
  - It collects `.v` files: a multi-file micro is run through its includer, and a header-only `.vh` never alone.
  - It sorts them **all together** by size, smallest first, and runs each under `--elab-only`, the bridge, or `B --one F`. `--top` comes from `micro-top` when a golden exists.
  - It prints one line per file and a summary.
  - An unlisted failure or a listed pass fails the run. A listed entry is `<relative path> <one-line reason>`.
  - An ASan `odin3` without `systemd-run --user` exits 77 (point (n)).
- `micro_expect_fail.txt` lists only:
  - the Parmys-rejected micros (PHASE2 #4, the 2C list's entries);
  - the three splicing micros (PHASE2 #11);
  - files needing a header the tree lacks.

  Each line has its reason.

**Tests:**
- `test_elab_sweep.py`, over fakes:
  - one ascending size order across two directories (review I12);
  - `--jobs 2 --mem-cap 10G` refused against a fake `/proc/meminfo` of 24 GB (2 × 10 > 14.4);
  - a listed pass → 1, an unlisted failure → 1, a listed failure → 0;
  - `--top` taken from a fake golden's first `.model`;
  - a child killed by its cap reported `out of memory (cap 4G)`;
  - an ASan binary without `systemd-run` → 77;
  - `--bench B` starts `B --one F` per file.
- `verilog_micro_elab` (539 files, `ODIN3_HEAVY_OK=1 ctest --preset sweep`): green with the list.
- `elab_equiv_micro` (`sweep`): green with the list, its run log quoted in the PR.
- `project_corpus_frontend`:
  - `auto_top`, `param_instance`, `param_string`, `top_param`, `single_file` and `multi_dir` are equivalent to their `ref.blif` through the bridge;
  - `neg_unresolved_module`, `neg_unknown_top`, `neg_ambiguous_top` and `neg_no_top` give their kind, location and names;
  - a case where the fixture's 0.69 `ref.cmd` and 0.55 disagree is reported `oracle drift`, not failed.
- `verilog_reject_2d` and `verilog_ignore_2d` are re-run here with 0 pending.
- **Oracle crash (PR 2D-3 final review m6):** Yosys 0.55 segfaults (core dumped) on recursive instantiation under `hierarchy -check -top` (`top` instantiating itself; `ping`→`pong`→`ping`), where 2D reports "recursive instantiation of 'top'". `elab-sweep --equiv` and `project_corpus_frontend` class a Yosys crash (signal exit) on such a case as `oracle crash`, not a mismatch or a 2D failure.

### Task 27: The VTR sweep, `bench_elab`, the PHASE2 entry (§14.5, §15)
**Files:**
- Create `tests/bench/bench_elab.c` (Release).
- Create `tests/elab/vtr_expect_fail.txt`, pre-listing `koios/lenet.v` for the bridge's Yosys side as a candidate `out of memory (cap 8G)` entry, removed if it passes (review M24).
- Modify `CMakeLists.txt`: CTests `verilog_vtr_elab` (label `sweep`, when `VTR_ROOT` is set), `elab_equiv_vtr` (labels `sweep` and `nightly`) and `bench_elab_smoke` (label `sweep`).
- Modify `.github/workflows/nightly.yml`: the VTR bridge, the 5,000-seed generator run and the 0.69 drift job, as commented steps until a runner with VTR exists.
- Modify `docs/PHASE2.md`: 2D's entry, the next free number.

**Consumes:** Tasks 21, 25–26; `odin3_ast_bytes_reserved`; the IR store ends.

**Produces:**
- `bench_elab --one <file>` prints one line for one file:
  - bytes and parse time;
  - stage-1, binding and stage-2 times, separately;
  - parsed and elaborated node counts and `odin3_ast_bytes_reserved` of each;
  - modules, specializations, cells, `$proc` nodes, `$mem` cells, nets, wires and pins;
  - each EL-46 estimate term and the estimated total;
  - peak RSS (`getrusage`).
- The bench set is `elab-sweep --bench build/release/bench_elab` over the **one set** of 64 files (`$VTR_ROOT/vtr_flow/benchmarks/verilog/*.v` and `koios/*.v` together), smallest first. `lenet.v` is last, alone under 8 GiB. It then prints the §15 targets, each `ok` or `MISSED`.
- `verilog_vtr_elab` runs the same one set under `--elab-only` with `vtr_expect_fail.txt`, as a `sweep` test (never in the `-j4` gate; ruling I12).
- `elab_equiv_vtr` classes a Yosys 0.55 crash on recursive instantiation as `oracle crash` (Task 26's rule; PR 2D-3 final review m6).

**Tests:**
- `bench_elab_smoke` runs `--one` on two micros and checks every column is present.
- `verilog_vtr_elab` (`ODIN3_HEAVY_OK=1 ctest --preset sweep`): green locally on all 64 files, with the run log quoted in the PR.
- Bench on the dev machine, targets from §15:
  - `mcml.v` stage 1 + stage 2 ≤ 0.5 s and peak RSS ≤ 200 MB;
  - `lenet.v` ≤ 10 s and ≤ 2.0 GB;
  - the 31 `verilog/*.v` files ≤ 3 s total, summed from the one set's lines.

  A missed target is investigated and reported with the estimate terms, never relaxed.
- `elab_equiv_vtr`: run once locally on the one set, smallest first, with its summary quoted in the PR; `lenet.v`'s Yosys side is reported with its peak RSS or its cap outcome.
- The PHASE2 entry records:
  - the bench output verbatim and the sweeps' summaries;
  - the divergence list §17 by reference, and the v4 spec amendments (EL-39 x defaults, EL-30 `~&`);
  - the erratum of point (h) (§14.1's stale OUT-sync phrase);
  - the spec v4.4 errata from the 2D-1 final review: §13's implementation note (the `write_rtlil` text 2D-1 ships, with `attribute \top 1`) and EL-47's ABI line (v4 kept, no bump);
  - the Parmys exceptions added to PHASE2 #4;
  - this plan's points (a)–(bb) as agent defaults;
  - ABI v4 kept unreleased, with the bump deferred to the first release or Phase-2 close as a decision item;
  - the three IR changes as "landed, for Peter's review";
  - the **2H backlog item**: `equiv-check --x-report`, its latch handling and exit-code contract, and the defined-output miter (§14.6 items 3–5), fed by `elab-equiv --a-only`'s netlists.

### Task 28: Crash fuzzer, differential generator, constant and cell differentials (§14.5)
**Files:**
- Create `tests/fuzz/fuzz_elab.c`, `tools/elab-check/elab-diff` (Python), `tools/elab-check/const-diff` (Python), `tests/unit/test_elab_cellsim.c` and `tests/tools/test_elab_diff.py`.
- Modify `tools/verilog-gen/verilog_gen.py` (`--elab`) and its test.
- Modify `CMakeLists.txt`:
  - `fuzz_elab` and the 10 s smoke `fuzz_elab_smoke` in the `ODIN3_FUZZ` block;
  - `elab_gen_vs_yosys` with 200 seeds and `elab_const_diff` with 500 expressions, both label `sweep` in the 0.55 block (review M24);
  - `nightly` labels for 5,000 seeds, 10,000 expressions and the 0.69 drift.

**Consumes:** Tasks 7, 21 and 25; 2C-2's generator and fuzz block; `odin3_elab_test_set_limits`; the cell `simulate` hooks.

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
- `elab-diff --seeds A..B [--yosys-069 Y2]` runs `elab-equiv` per seed. A non-equivalence fails and keeps the seed and file. With `--yosys-069`, a file where 0.55 and 0.69 disagree is listed as `oracle drift`, not failed. Yosys 0.55 segfaults on recursive instantiation (PR 2D-3 final review m6): a generator case that recurses and makes Yosys crash is listed as `oracle crash`, not failed (2D must still give its recursion error).
- `const-diff --count N --seed S` generates random constant expressions over Task 7's ops and compares `odin3_cval_op` through a tiny driver against Yosys 0.55's values.

**Tests:**
- `test_elab_diff.py`:
  - deterministic per seed;
  - each trap feature appears in at least one of seeds 1–50;
  - drift is classified, not failed.
- `elab_gen_vs_yosys` (`sweep`) passes on 200 seeds. A local 5,000-seed run's summary is quoted in the PR.
- `elab_const_diff` (`sweep`) passes on 500 expressions.
- `fuzz_elab_smoke` runs 10 s with no crash (clang).
- `test_elab_cellsim` (review M18): for every word cell that has both a `simulate` hook and an `odin3_cop` mapping, 1,000 random 0/1 cases at widths 1, 7, 64 and 65, with random signs. `odin3_cval_op` and the hook agree bit for bit. The cells are `$add $sub $mul $and $or $xor $not $eq $ne $lt $le $gt $ge $reduce_and $reduce_or $reduce_xor` plus Task 1's `$neg $xnor $reduce_xnor $reduce_bool $logic_not $logic_and $logic_or`. `$mux`, `$pmux`, flip-flops and memories are out of scope.
