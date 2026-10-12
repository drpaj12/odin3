# 3D Linked ABC — Implementation Plan (v2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

v1 was the agent draft, written with the 3E and 3H plans against one shared contract (PR order, the heavy-run convention, the `$lut`, ISOP and run-information hand-overs).

v2 applies the pre-flight review `3-plans-review` (3D: 0 critical, 6 important, 13 minor; every finding applied, no minor rejected) and the controller's rulings I3–I10; each change cites its finding or ruling where it lands. The main ones: 2E Task 1's cells land before `$lut` (I5, conflict note 2); one budget message (I6, point (i)); only the clean `ODIN3_ERR_ENGINE` return ships and 3D-2 waits for Peter (I7, point (a)); word-cell BLIFs leave the `abc` inputs (I8, point (j)); `abc-equiv --step` (I9); 3H's nightly runs the 3D sweeps (I10, point (m)); no tool sets `ODIN3_HEAVY_OK`, and every heavy run waits for 2E-1 (I3, I4).

**Goal:** Link the pinned ABC (`a3001b72`) into Odin III without letting its `exit()` calls or global symbols into `libodin3`, and add the core `abc` pass. The pass maps each module's soft logic to `$lut` cells, one ABC run per clock domain, re-stitches the result into the IR with provenance from ABC's own correspondence, and gives byte-identical reruns. In order of landing:
- `$lut`, first and alone (3D-1), so 3E-5 can build on it;
- the adapter `libodin3_abc.so` with its engine ABI (3D-2);
- pass-run arguments and run information, the IR-12 amendment (3D-3);
- the script checker (3D-4) and the project/CLI `abc_script` (3D-6);
- the pass on the BLIF exchange, which with 3E and 3F gives **M1** (3D-5);
- the in-memory MiniAIG/MiniLUT exchange after the spike (3D-7);
- the evidence: determinism, `bench_abc`, the large-design schedule (3D-8).

No `lower`, no `hardmap`, no `write_blif --vtr` (3E), no VTR wrapper (3F), no subprocess engine, no retiming.

**Architecture:** Three layers, each with one owner of its state.
- `adapters/abc/` (C17, `libodin3_abc.so`) is the only code that touches ABC.
  - `bridge.c` alone includes ABC headers.
  - `session.c` holds the one static session: a fresh `Abc_Frame` per run, the generators reset, `srand(1)`.
  - `guard.c` holds the link-time `--wrap` wrappers, the `setjmp` guard and the thread check, plus the switchable `ODIN3_ADAPTER_ASSERT`.
  - `capture.c` redirects fds 1 and 2 into a `tmpfile()` during each ABC call.
  - `adapter.c` fills the seven-function `odin3_abc_engine` table and registers it through the public ABI.
- `src/passes/abc/` is a core built-in pass that never includes ABC. It reaches ABC only through the registered engine (or the test double).
  - Before ABC starts, `script.c` checks the whole script against the alias table (`alias.c`) and the allow-list (`allow.c`).
  - Per module, `region.c` classifies cells (soft, register, constant, cut) and computes CIs, COs, register correspondences and the clock domains as flip-flop sets.
  - Per domain, the run goes through one exchange: `xblif.c` (BLIF in a directory, read back with 1C's reader into a scratch design) or `aig.c` (MiniAIG in, MiniLUT out). Both give one `abc_result` (`result.c`, constants folded).
  - `splice.c` re-stitches in four phases (A allocate, B re-stitch, C delete and move names, D cleanup) and restores init-1 registers.
  - `prov.c` builds anchors and regions and makes the `DERIVED` records. `depth.c` computes the LUT depth for the run information.
- `src/ir/prov.[ch]` gains pass-run arguments and run information. `src/ir/cells/lut.c` is the `$lut` cell, with `src/ir/lut.h` for its table helpers. `src/util/sha256.[ch]` gives the script identity.

**Tech Stack:** C17 with the `odin3_flags` set, `src/util`, `src/ir` (+ `$lut`, IR-12), the 1C BLIF reader (BLIF read-back into a scratch design), 1E simulate hooks and `odin3_prng`, the 1D pass manager and ABI, 1G tech-library cells (cut), the 2B project reader, 3E-5's `odin3_blif_isop`, and 2E-1's `odin3_budget_check`, `tools/heavy-run`, `tools/build-locked` and `odin3_add_sweep_test`.

ABC at `a3001b72edc5de22442e942165487fddf150e3d0` (`third_party/abc`, CMake target `libabc-pic`, linked through the C++ driver). GNU ld `--wrap` and version scripts, pthreads, Unity, libFuzzer (clang), Python 3 stdlib (`abc-equiv`, `abc-diff`), `tools/equiv-check` (ABC `dsec`), Icarus (the Verilog `$lut` check). VTR `3c9a4d23`'s ABC stage is the reference.

**Spec:** `docs/specs/2026-10-11-3D-linked-abc-design.md` **v2**. It is binding: AB-n and § numbers below refer to it. Its §11 texts and §8.5's IR.md text are verbatim requirements.

Also:
- `docs/PHASE3.md`: decisions 1 and 8–14 (P3-n), 10, 16–18; the 3D row and checklist; risk 1.
- The 3E spec v2.1 (`docs/specs/2026-10-11-3E-blif-vtr-design.md`): §4.2 and §14.1 are requirements on 3D; §5 item 1 prints the run information; §9 the `unconn` classes.
- The 3H spec v2.1: §4.1 (`abc_depth`), §3.4 (P′), §7.1 (one heavy convention).
- `docs/DESIGN.md` §4.0, §5.2, §6, §15; `docs/IR.md` IR-4, IR-6, IR-8 to IR-13, IR-15 to IR-17 and §5.3 (names in output).
- The 1C, 1D, 1E and 1G specs; the 2E plan's point (p) and Task 3 (`src/passes/budget.[ch]`).
- The standing rules, including the heavy-run rule and its slice correction.

**Ordering and bases.** `main` (c1e3987) has 2D-1 (#54), this spec (#53), the 2E and 2F plans (#52), the 3E and 3H specs (#51) and 2C-1 (#49). In flight:
- 2C-2 (`feat/2C-read`): `read_verilog`, `--parse-only`, `src/cli/main.c` (~100 lines), `src/passes/builtin.c`, `docs/PASSES.md`, `CMakeLists.txt`.
- 2D-2 (`feat/2D-const`): `src/frontends/elab/` only, plus `CMakeLists.txt` and `pyproject.toml` appends.
- 2E-1 (`feat/2E-core`): Task 2 is in review. It brings `tools/heavy-run`, `tools/build-locked`, `cmake/Odin3Sweep.cmake` (`odin3_add_sweep_test`) and `ODIN3_HEAVY_RUN`. Its Tasks 1 (IR change D, eleven registry cells) and 3 (`src/passes/budget.[ch]`) follow. **2E-1 merges to main first, and every heavy task of this plan depends on it** (point (m)).

Nine PRs, as spec §15. 3D-0's code is never merged.

| PR | Branch | Base | Tasks | Waits for |
|---|---|---|---|---|
| 3D-0 spike | `~/odin3-ws/work/spike-abc/` (findings: `docs/3D-spike`) | — | 1 | **Nothing; starts now** (the program and the micro checks). Its runs on the five VTR goldens go through `tools/heavy-run` (2E-1 on `main`). The findings PR is docs-only (point (f)). |
| 3D-1 `$lut` | `feat/3D-lut` | `origin/main` now; rebased onto `main` after 2E-1 | 2 | **Starts now; merges after 2E-1** (2E Task 1's IDs 63–73 land first, ruling I5). Peter reviews it (IR change). 3E-5 starts the day it merges. |
| 3D-2 adapter | `feat/3D-adapter` | `origin/main` now | 3–6 | **Starts now; merges only after Peter rules on point (a)'s question** (ruling I7). Rebase onto `main` before Task 6 if 2C-2 has merged (`main.c`, `CMakeLists.txt`). Peter reviews it (adapter build, ABI). |
| 3D-3 run records | `feat/3D-records` | `main` after 3D-1 | 7–8 | 3D-1. Peter reviews it (IR-12, ABI). Task 9 moved to 3D-5 (finding 12). |
| 3D-4 script | `feat/3D-script` | `main` after 3D-3 | 10–11 | 3D-3 (`sha256`). Task 11's two adapter tests land with whichever of 3D-2 and 3D-4 merges second (point (p)). |
| 3D-6 project and CLI | `feat/3D-project` | `origin/main` now | 12–13 | **Nothing; starts now** (finding 11). The first of 3D-2, 3D-3 and 3D-6 to merge writes the "ABI v4 (cont., 3D)" line. Light review (P3-1). |
| 3D-5 pass, BLIF exchange | `feat/3D-pass` | `main` after 3D-6 and 3E-5 | 9, 14–19 | 3D-2, 3D-3, 3D-4, 3D-6 (point (e)), 3E-5 (`odin3_blif_isop`), and 2E-1 on `main` (`odin3_budget_check`, `odin3_add_sweep_test`, `tools/heavy-run`). Its golden sweeps also need 3H-1's `qor_heavy` (finding 22). M1 therefore waits on Peter's point (a) ruling through 3D-2. |
| 3D-7 in-memory exchange | `feat/3D-aig` | `main` after 3D-5 | 20–21 | 3D-5 and the 3D-0 "go" (criteria 1, 2, 4, 6, 7). Peter only if the spike forces P3-9 (c) or observation outputs. |
| 3D-8 evidence | `feat/3D-evidence` | `main` after 3D-5; rebased after 3D-7 | 22 | 3D-5 (3D-7 for the in-memory rows); 2E-1 (heavy runs). Opens last. |

Why 3D-1, 3D-2 and 3D-6 start now:
- 3D-1 touches `src/ir/cells/{lut.c,builtin.c,cells.h}`, a new `src/ir/lut.h`, `tests/unit/test_ir_cells.c`, `test_ir_proc.c`, `docs/IR.md`'s cell paragraphs and DESIGN §5.2. In flight, only 2E Task 1 (7e6c1c9 on `feat/2E-core`) appends to the same registry; it merges first, and conflict note 2 settles the order.
- 3D-6 touches only the project reader, the corpus, `main.c`'s option scan and one ABI function (conflict notes 3, 7, 8).
- 3D-2 touches the `ODIN3_WITH_ABC` block of `CMakeLists.txt`, `adapters/abc/`, `odin3.h`'s status enum and a new engine section, `src/api/`, `src/passes/abc/engine.c`, the symbol tools and two `main.c` hunks. 2C-2's `main.c` hunks are elsewhere (`--parse-only`), so the second to merge keeps both (conflict note 7).

**Model/effort** (PHASE0 §9; PHASE3 row 3D: Opus `xhigh` → `high`; spec §15):

| Task | Implementer | Reviewer |
|---|---|---|
| 1 Spike (AB-17's eight criteria) | Opus `xhigh` | Opus `high`; Peter reads the go/no-go |
| 2 `$lut` cell, JSON form, IR.md/DESIGN text | Opus `high` | **Fable** (IR change; Peter reviews too) |
| 3 ABC build: `libabc-pic`, pin, `-O2`, history off, the PIC `abc` tool | Opus `high` | Opus `high` |
| 4 Engine ABI, `ODIN3_ERR_ENGINE`, the registry | Opus `high` | **Fable** (ABI; Peter reviews too) |
| 5 Guard, wrappers, thread check, capture, session, exports, symbol checks, `ODIN3_ADAPTER_ASSERT` | Opus `xhigh` | **Fable** |
| 6 Bridge calls, test hooks, CLI init, `--version`, NOTICE and licence scan | Opus `high` | Opus `xhigh` |
| 7 SHA-256 | Opus `high` | Opus `high` |
| 8 Pass-run arguments and run information (IR-12) | Opus `high` | **Fable** (IR; Peter reviews too) |
| 9 Verilog `$lut` | Opus `high` | Opus `high` |
| 10 Alias table and allow-list data | Opus `high` | Opus `high` (re-derives every row at the pin) |
| 11 Script checker, default script, fuzzer, adapter checks | Opus `high` | Opus `xhigh` |
| 12 `abc_script` record, reader, dump, reference parser, fixtures | Opus `high` (bulk `"abc_script": null`: Sonnet `medium`) | Opus `high` |
| 13 `--abc-script`, `odin3_pass_set_abc_script` | Opus `high` | Opus `high` |
| 14 Arguments, precedence, the run loop, logs, budget, pass row | Opus `high` | Opus `high` |
| 15 Region: classification, constants, CIs/COs, domains, loops | Opus `xhigh` | Opus `xhigh` |
| 16 Fake engine, `abc_result`, BLIF exchange | Opus `high` | Opus `xhigh` |
| 17 Splice and restore | Opus `xhigh` | **Fable** |
| 18 Provenance, depth, run information | Opus `xhigh` | **Fable** |
| 19 Real-engine integration, `equiv-check --blackbox`, `abc-equiv`, fuzzer | Opus `high` | Opus `high` |
| 20 AIG builder and the in-memory protocol | Opus `xhigh` | Opus `xhigh` |
| 21 Aligned MiniLUT, name map, `abc-diff`, the default flip | Opus `xhigh` → `high` | **Fable** |
| 22 Determinism, `bench_abc`, calibration, H1–H6, PHASE3 entry | Opus `high` (runs: Sonnet `high`, background + Monitor) | Opus `high` |
| Whole-branch review before **each** PR (3D-1 … 3D-8, and 3D-0's findings) | — | **Fable**, else Opus `xhigh` |

Effort cues:
- Say `/model opus` + `/effort xhigh` at the start of Task 1, Task 5, Tasks 15, 17 and 18, and Tasks 20–21.
- Say `/effort high` for Tasks 2–4, 6–14, 16, 19 and 22.
- Say `/model sonnet` + `/effort medium` for Task 12's bulk fixture edit, and `/model sonnet` + `/effort high` for Task 22's runs (started in the background, watched with Monitor).
- Say `/clear` between tasks (one task per session).

**Lanes** (one task at a time on each branch):
- **Now:** Task 1 (3D-0) ‖ Task 2 (3D-1) ‖ 3D-2: 3 → 4 → 5 → 6 ‖ 3D-6: 12 → 13. Tasks 4 and 5 share `src/passes/abc/engine.c`'s header, and Task 6 extends Task 5's session.
- **3D-3:** 7 ‖ 8 (disjoint files).
- **3D-4:** 10 → 11. Task 11 parses with Task 10's tables.
- **3D-5:** 9 → 14 → 15 → 16 → 17 → 18 → 19. Tasks 14–19 share `run.c` and the fake engine; Task 9 (the Verilog writer) is independent of them.
- **3D-7:** 20 → 21, after the spike's go.
- **3D-8:** 22.
- Across plans: 2E-1 → 3D-1 → 3E-5 → 3D-5. 3D-5 → 3E's header loop (point (g)) and 3H-3's P′ leg (M1). 3H-1 → 3D's golden sweeps.

**Pre-flight conflict note.**
1. **`CMakeLists.txt`.** Every task appends sources to `odin3_core`, `odin3_add_unit_test(...)` lines and CTests. These are append-only hunks; keep both sides.
   - Task 3 rewrites the `ODIN3_WITH_ABC` block. `odin3_add_abc()` gains ABC's flags, `ABC_NO_HISTORY` and the pin check (`cmake/abc_pin.cmake`). `odin3_abc_exe` (OUTPUT_NAME `abc`) replaces ABC's own `abc` target as `abc_tool`'s dependency, so the two `$<TARGET_FILE:abc>` uses (the `writer-check` and `python_tools` environments) become `$<TARGET_FILE:odin3_abc_exe>`. Nothing in flight edits this block.
   - Task 5 adds the adapter target `odin3_abc`, `test_abc_adapter` and the symbol CTests. `symbol_rules` gains one argument.
   - In flight: 2C-2 (~54 lines), 2D-2 (15) and 2E-1 (14: `ODIN3_HEAVY_RUN` and the `Odin3Sweep.cmake` include) append elsewhere.
   - Every heavy CTest of this plan goes through `odin3_add_sweep_test()` (2E-1), never a bare `add_test`. Fuzz targets go inside the existing `ODIN3_FUZZ` block.
2. **The cell registry: `src/ir/cells/builtin.c`, `cells.h`, `tests/unit/test_ir_cells.c`, and `test_ir_proc.c`'s count check.**
   - Ruling I5: 2E Task 1 (IR change D, 7e6c1c9 on `feat/2E-core`) lands first, with its eleven cells at IDs 63–73 after `$proc` (count 74). 3D-1 rebases onto it and appends `&odin3_cell_lut` after them, at the next free ID (74 today; count 75).
   - No 3D test hard-codes the `$lut` ID: they get it through `src/ir/lut.h`'s `odin3_lut_builtin_index()`. Task 2 raises the two count constants, `BUILTIN_COUNT` in `test_ir_cells.c` and in `test_ir_proc.c` (line 40), from 74 to 75 (finding 13).
   - Append-only. The 2E plan needs no amendment.
3. **`include/odin3/odin3.h`** stays at **ABI v4** (spec Q4; point (d)):
   - Task 4 adds `ODIN3_ERR_ENGINE = 8` before `ODIN3_STATUS_COUNT`, which becomes 9, and the engine section verbatim from §4.3. The first of Tasks 4, 8 and 13 to merge writes the history line `ABI v4 (cont., 3D): …`; each adds its words ("ODIN3_ERR_ENGINE and the ABC engine (odin3_abc_engine_register)" for Task 4).
   - Task 8 adds `odin3_design_get_passrun_count` and `odin3_design_get_passrun` ("pass-run arguments and run information").
   - Task 13 adds `odin3_pass_set_abc_script` ("odin3_pass_set_abc_script").
   - In flight: 2D Tasks 10 and 17 (2D-3, 2D-5; not started) extend the "(cont., 2D)" line. 2C-2, 2D-2 and 2E add nothing to `odin3.h`.
   - All hunks are additive. The second to land keeps both lines.
   - `src/api/version.c`'s `odin3_status_string` table gains `"ODIN3_ERR_ENGINE"`. The test that asserts `ODIN3_STATUS_COUNT == 8` (`tests/unit/test_api.c:36`) moves to 9 in Task 4 (finding 14).
4. **`src/ir/prov.[ch]` and `src/passes/manager.c`.** Task 8 adds the run rows' `args` and `info`, `odin3_pass_run_begin_args` and the accessors, and makes `open_run` intern the trimmed arguments. 2E Task 3 adds `odin3_prov_bytes_reserved` to `prov.[ch]` in another hunk. 2D Task 4's refusal sits before `open_run` in `run_def` and is untouched.
5. **The pass table: `src/passes/builtin.c` and `builtin.h`.**
   - Task 14 adds `&odin3_pass_abc`, defined in `src/passes/abc/run.c` with `processes` = `ODIN3_PASS_PROCESSES_REFUSE` and `wants_ast` false (point (n)). Task 19 adds `odin3_design_compact` to `builtin.[ch]` if 2F Task 13 has not (point (y)).
   - 3E-1 rewrites `WRITE_BLIF`'s help and option parsing.
   - In flight: 2C-2 adds `read_verilog`. Later: 2E Task 3 adds `proc`, 2F Task 4 eight opt passes, and 2F Task 13 factors `compact`.
   - Table entries are independent lines; keep both sides.
6. **`docs/PASSES.md`.** Task 14 adds the `abc` row with spec §7.1's text exactly. 3E-1 rewrites the `write_blif` row. 2C-2, 2D, 2E and 2F add rows. Rows are independent lines.
7. **`src/cli/main.c` and `tests/cli/test_cli.sh`.**
   - Task 6 adds `odin3_abc_adapter_init()` under `ODIN3_HAVE_ABC` before any `--plugin`, its warning, and the `--version` line.
   - Task 13 adds `--abc-script` to `k_value_options` and `cli_state`, the usage line and the `odin3_pass_set_abc_script` call.
   - In flight: 2C-2 adds `--parse-only`. Later: 2D Task 17 adds `--elab-only`, 2F `--opt-snapshot`.
   - All additive; the second to land keeps both.
8. **The project grammar `abc_script` (Task 12 only).**
   - Files: `src/frontends/project/{project.h,project.c,o3proj.c,dump.c}`; every positive `tests/golden/projects/*/expected.json` (`"abc_script": null` after `"flow"`, one mechanical commit); `tests/golden/projects/README.md`; four new case directories; `tools/project-fixtures/project_fixtures.py`; DESIGN §4.0.
   - In flight: 2C-2 only adds `.o3proj` inputs under `tests/unit/data/verilog/read/` and never edits the reader. 2D Task 26 adds `--with-frontend` to `project_fixtures.py`, in another function.
   - 3E and 3H do not touch the project grammar.
9. **Writers.**
   - `src/backends/verilog/writer.c`: Task 9 (in 3D-5) adds `$lut` (`FORM_LUT`). 2E makes no edit (2E conflict note 5).
   - `src/backends/json/*`: no edit. The generic path writes `$lut`; Task 2 only asserts the form.
   - `src/backends/blif/{writer,vtr,isop}.[ch]` belong to 3E. 3D's own exchange writer is `src/passes/abc/xblif.c`.
   - The `# abc:` header loop in `vtr.c`'s header function lands with whichever of 3E-1 and Task 8 merges second (point (g); finding 18).
10. **`src/passes/budget.[ch]`** (2E Task 3). Task 14 makes `odin3_budget_limit()` the smaller of 60% of physical memory and the smallest numeric cgroup `memory.max` on the process's cgroup path, changes the one shared message, adds a fake-cgroup hook, and updates the 2E/2F tests that assert the old text or limit (point (i); ruling I6). 2F only calls the budget; if 2F lands later, its tests use the new text.
11. **Tools.**
    - Task 5 alone edits `tools/check-symbols.sh` (`--only`/`--forbid` mode), `tools/check_rules.py` (the `assert(` rule only; no trap allowance before Peter rules, point (a)) and `tools/lint.sh` (it covers `adapters/abc/*.c`).
    - Task 19 adds `--blackbox` to `tools/equiv-check/equiv_check.py`. 3H calls `equiv-check --timeout`; 2H later adds `--x-report`. These are independent options.
    - `pyproject.toml` (`[tool.ruff] src`, `[tool.mypy] mypy_path`) and `tests/tools/__init__.py`: Task 19 appends `tools/abc-check`. 2D-2 and 2E-1 (`tools/proc-check`), 3E and 3H append theirs. Keep every entry.
12. **Docs**, different paragraphs each: `docs/IR.md` (Task 2's "LUT cells (3D)" after "BLIF cells (used by 1C)"; Task 8's IR-12 row, two paragraphs and §11 entry); `docs/DESIGN.md` (Task 2 §5.2, Task 19 §6 step 10, Task 12 §4.0; 3E-1 §9's BLIF row); `docs/PHASE3.md`, append-only (3D-0's findings, Task 21's ratios, Task 22's entry); `docs/ORACLES.md` and `CLAUDE.md`'s Layout (Task 3); the corpus README (Task 12).
13. **3E's files that 3D consumes.**
    - `src/backends/blif/isop.[ch]` (3E-5): Tasks 9, 16 and 20 call `odin3_blif_isop`.
    - `write_blif --vtr` (3E-1): `abc-equiv --vtr` (Task 19) and the M1 check.
    - The `--vtr` writer's `$lut` row and the plain writer's `$lut` → `.names` are 3E-5's.
14. **3H consumes**:
    - the run information's `depth=` and `exchange=` words, through 3E's `# abc:` header;
    - the INFO line `abc: final: depth <d>`.
    - Their formats are fixed in Task 18 and must not change without 3H.
15. **Heavy runs.** 2E-1's `tools/heavy-run`, `tools/build-locked`, `cmake/Odin3Sweep.cmake` and `ODIN3_HEAVY_RUN` are consumed, never copied or cherry-picked. Every heavy item depends on 2E-1 being on `main`: the spike's golden runs, Task 19's sweeps, Task 21's `abc-diff`, Task 22's suite, and H1–H6. The Python tools' golden mode also needs 3H-1's `qor_heavy.require_heavy` (point (m)).
16. **`third_party/abc`** is read only. Task 3's pin check stops configure on any other commit, and a pin bump is an ORACLES change (Peter).

**Spec points made exact here** (agent defaults, reversible; Task 22 logs them in PHASE3):

- (a) **`ODIN3_ADAPTER_ASSERT` returns cleanly; traps wait for Peter** (ruling I7). §4.1 has the adapter trap (`__builtin_trap()`) as a documented exception to CLAUDE.md's "trap only in `check`". That exception, and any `check_rules.py` allowance for it, is Peter's call (CLAUDE.md rule 6), so only the clean return ships:
  - `adapters/abc/guard.h`: `#define ODIN3_ADAPTER_ASSERT(c) do { if (!(c)) { return odin3_adapter_assert_fail(__FILE__, __LINE__, #c); } } while (0)`. There is no trap arm, no CMake option and no trap test.
  - `odin3_status odin3_adapter_assert_fail(const char *file, int line, const char *expr)` records the session's pending failure `{ODIN3_ABC_FAIL_ASSERT, 0, "adapter: <file>:<line>: <expr>"}` (truncated to 255 bytes), disables the engine as an escape does, and returns `ODIN3_ERR_ENGINE`. The pass then reports `abc: ABC assertion failed during '<cmd>': adapter: …; linked ABC is disabled until odin3 restarts`.
  - The macro may appear only in functions returning `odin3_status`; a use in a `void` function is a compile error under `-Werror`.
  - **The off-thread escape** (AB-6: ABC's own thread calls `exit`/`abort`/`__assert_fail`) has no caller to return to. Its wrapper writes the stderr line and then calls `adp_offthread_stop()` in `guard.c`, whose body is Peter's answer. Until he rules, its body is left out and 3D-2 does not merge.
  - The 3D-2 PR asks him one numbered question: "(1) Allow `__builtin_trap()` in `adapters/abc/guard.c`'s off-thread escape (required to stop safely), with the matching `check_rules.py` allowance? If not, the alternative is a non-trap stop after the line, `raise(SIGKILL)`. (2) Optionally allow the trap for adapter invariants (`ODIN3_ADAPTER_ASSERT`), instead of the shipped clean return?"
  - Either answer for (1) is a one-function change plus Task 5's `o3test_thread_exit` signal (`SIGILL` or `SIGKILL`). A "yes" to (2) is a one-line macro change in review.
- (b) **Where the engine lives.** `src/passes/abc/engine.c` holds the process's engine copy. Internal `const odin3_abc_engine *odin3_abc_engine_get(void)` returns it, or NULL.
  - `src/api/abc.c` holds the public `odin3_abc_engine_register`, which validates the table and then calls `odin3_abc_engine_set`.
  - `odin3_abc_test_set_engine` (`abc_test.h`) overrides it for tests; NULL restores the registered one. It is the only way past "final".
  - The texts that §4.3 leaves open, each after `odin3_abc_engine_register: `:
    - `invalid argument engine`
    - `engine version <v>, expected 1`
    - `engine '<name>' has no '<fn>' function`
    - `engine '<name>': reserved[<i>] must be NULL`
    - `engine '<name>': abc_commit '<s>' is not 40 lower-case hex digits`
    - `abc engine already registered (<name>)`
- (c) **`$lut` helpers live in `src/ir/lut.h`.**
  - §2 puts `ODIN3_LUT_MAX_WIDTH` in `src/ir/cells/cells.h`, which is private to `src/ir/cells`. The pass, the writers and 3E need it, so `src/ir/lut.h` holds it with the parameter and port indices, the table helpers and the registry lookup `odin3_lut_builtin_index()` (Task 2). `cells.h` includes `lut.h`, so the constant is in `cells.h` as §2 says.
  - Registry order is conflict note 2's (ruling I5).
- (d) **ABI line.** One new history line, `ABI v4 (cont., 3D): …`, written by whichever of Tasks 4, 8 and 13 merges first and extended by the others. `ODIN3_ABI_VERSION` stays 4 (Q4), and `cli_version` still prints `(ABI 4)`.
- (e) **Script precedence needs 3D-6, so 3D-5 waits for it.** §15 puts "args and precedence" in 3D-5. But precedence steps 2–3 read the project record's `abc_script` and `odin3_pass_options.abc_script`, both of which Tasks 12–13 add. 3D-6 is light and waits for nothing (finding 11), so it lands first, and 3D-5 waits for it.
  - The CTest `cli_abc_script_precedence` (§10.3) needs the pass, so it moves to Task 19.
- (f) **3D-0's findings.** The spike's program stays in `~/odin3-ws/work/spike-abc/` and is never committed. Its eight criteria and fallbacks become a PHASE3 entry, "3D-0 spike (AB-17)", in a docs-only PR (`docs/3D-spike`) that the controller merges. 3D-7 starts only once it has merged with "go".
- (g) **The `# abc:` header lines** (3E §5 item 1) need both 3E-1's VTR header (`vtr.c`'s header function) and Task 8's `odin3_passrun_info`. Whichever of 3E-1 and 3D-3 merges second adds the loop to `vtr.c`: one `# abc: <run information>` per pass run named `abc` with non-zero information, in run order, plus the test `vtr_header_abc_runs` in `tests/unit/test_blif_vtr.c`. Until then `--vtr` writes no `# abc:` line.
- (h) **First location of a LUT** (3E-6, 3E §10 item 7). IR.md §5.3 already defines it: for `DERIVED`, the first leaf of the backward walk in parent order. Task 18 fixes the parent order: an old region's cells in node-ID order, and a multi-region LUT's extra record's parents in anchor order. So the rule is deterministic, 3D adds no new rule, and 3E-6 cites IR.md.
- (i) **Budget under a slice.** §7.8 reads `/sys/fs/cgroup/<own cgroup>/memory.max`. Under `tools/heavy-run` the job is in a scope whose `memory.max` is `max`, while the 6G cap sits on `odin3-heavy.slice`.
  - So `odin3_budget_limit()` walks the `0::<path>` line of `/proc/self/cgroup` up to the root and takes the smallest numeric `memory.max`.
  - The limit is the smaller of that and 60% of physical memory. With no cgroup v2 file, it is 60% alone.
  - This is the same walk as 3H's `qor_heavy.verify_scope`.
  - **One message for every pass** (ruling I6). `odin3_budget_message` gives `<pass> needs more than <N> bytes (the design's budget is <B> bytes: the smaller of 60% of physical memory and the cgroup's memory.max)`, replacing 2E's `… (the design's budget is 60% of physical memory)`. The text names the pass, so for `abc` it starts `abc needs …`, as §11 has it, with no separate `abc: ` prefix.
  - The tests that change with it (Task 14): 2E's `budget_e6`, `check_and_message` and `limit_default` (`tests/unit/test_budget.c` and `test_proc_pass.c`), and 2F's `budget` and `budget_opt` when 2F has merged (else the 2F implementer uses the new text). `limit_default` points `odin3_budget_test_set_cgroup_root` at an empty fake tree, so it stays exactly 60% of physical memory on any machine.
  - 2E §15 E6's and 2F's budget texts get an erratum in Task 22's PHASE3 entry.
- (j) **What "micros" means in CI.**
  - §13.4's CI set "`tests/micro` and `tests/golden/blif`" cannot run in Phase 3 lane 1: `tests/micro` holds Verilog, and `abc` needs a netlist.
  - `abc_equiv_micro` (gate) therefore runs the committed BLIFs: `tests/golden/blif/*.blif` and `tests/unit/data/abc/*.blif`.
  - **Word-cell BLIFs are filtered out** (ruling I8; finding 8). `tests/abc/equiv_expect_fail.txt` lists `tests/golden/blif/pow.parmys.blif views` (it reads as the word cell `$pow`, so `abc` stops with the views error) and, if it fails `--blackbox`, `tests/golden/blif/dffsre.parmys.blif blackbox` (an undeclared `$_DFFSR_PPP_`). `abc-equiv` passes `views` entries only when the views error appears. `test_abc_pass` and `abc_determinism_ci` skip every listed file. The golden sweeps use the same file, extended per run.
  - The micros' Parmys goldens (the k6 `regression/` tree) and the `vtr/` goldens run in `abc_equiv_golden` (`sweep`): every k6 Parmys golden except `mcml` and `LU32PEEng`, smallest first.
  - `mcml` and `LU32PEEng` run only in the H1–H6 slots.
- (k) **`<domain>` and clock names.**
  - `<domain>` is spelled `clock '<net>' posedge|negedge`, `global clock`, or `no flip-flops`.
  - `<net>` is the clock net's own name, else its primary wire bit (`w[k]`, or `w` for a 1-bit wire), else `$n<ID>`.
  - The domain sort key (§5.3) uses the same bytes, and the empty string when the net has no name.
- (l) **Runs, logs and directories.**
  - Run *r* counts from 0 across one `abc` invocation, in module order and then domain order.
  - `--log` writes `# abc run <r>: module '<m>', <domain>` before each run's raw output.
  - The default BLIF directory is `mkdtemp("$TMPDIR/odin3-abc-XXXXXX")`, with `/tmp` when `TMPDIR` is unset or empty. The pass unlinks the files it wrote (`abc<r>.in.blif`, `.cmd`, `.log`, `.out.blif`) and then `rmdir`s the directory. It never walks a tree recursively.
- (m) **Heavy items and their names.**
  - Sweep CTests, all through `odin3_add_sweep_test(… LABEL sweep …)`:
    - `abc_equiv_golden` (BLIF exchange; Task 19), and `abc_equiv_golden_aig` (Task 21);
    - `abc_diff_golden` (Task 21);
    - `abc_determinism_golden` (Task 22);
    - `bench_abc_set` (Task 22);
    - `fuzz_abc_script_long` and `fuzz_abc_splice_long` (past CI's 60 s and 2,000 cases).
  - The controller runs them: `tools/build-locked --preset release -j6`, then `ODIN3_HEAVY_OK=1 ctest --preset sweep -R '^abc_|^bench_abc|^fuzz_abc'`, or, before 2D Task 25 adds the `sweep` preset, `ODIN3_HEAVY_OK=1 ctest --preset release -L '^sweep$' -R '<same>' -j1`.
  - **The spec's nightly 3D runs** (§13.4 golden equivalence, §13.5 `abc-diff`, §13.8 (a), (c) and (d)) have one runner (ruling I10): 3H's hand-started nightly, step "3D sweeps" (3H Task 13). It runs `abc-equiv`, `abc-diff` and `abc-determinism --mode a|c|d` as direct commands inside its held lock (no nested `heavy-run`), when `src/passes/abc/` or `adapters/abc/` changed since the last such night, else weekly. There is no `nightly` label and no `nightly.yml` edit.
  - **No heavy tool sets `ODIN3_HEAVY_OK`** (ruling I3). `abc-equiv`, `abc-diff`, `abc-determinism` and `bench_abc` pass the caller's environment through unchanged. Given a path under `~/odin3-ws/golden`, the Python tools first call 3H's `qor_heavy.require_heavy()` (3H Task 1): outside `tools/heavy-run` they refuse with exit 2 (finding 22). A child they start that might itself be heavy gets the lock descriptor (`pass_fds=(9,)`), so `heavy-run`'s nesting rule applies instead of a wait on the parent.
  - Every heavy command runs under `timeout 3h`.
  - `mcml` and `LU32PEEng` (H1–H6) are never in a CTest. The controller starts them by hand, one per slot: `ODIN3_HEAVY_OK=1 tools/heavy-run timeout 3h <cmd>`.
  - A slot needs `--xl` (`odin3-heavyxl.slice`, 9G, `ODIN3_HEAVY_XL_OK=1`, idle machine only; the name has no dash, because systemd nests dashed slice names) only after `bench_abc` has measured more than 6 GB. That measurement goes to Peter first.
- (n) **The pass definition.** `odin3_pass_abc` is defined in `src/passes/abc/run.c`, with `wants_ast` false and `processes` = `ODIN3_PASS_PROCESSES_REFUSE`. `abc` needs the netlist view, and a design with processes has none.
  - A design without a top runs every module, and records `depth=0`.
- (o) **Adapter headers.** §14 names `bridge.h`, `guard.h` and `odin3_abc.h`. CLAUDE.md asks for one header per module, so `session.h` declares the session and the capture as well.
- (p) **Adapter tests that need the checker** — "no files" (it runs every allowed command) and "checked is what runs" (it compares `CmdSplitLine` with the checker's words) — are written in Task 11 and land with whichever of 3D-2 and 3D-4 merges second, so 3D-4 does not wait on Peter's ruling.
- (q) **The Verilog `$lut` form** (Task 9) is `assign <y> = <expr>;`, built from `odin3_blif_isop`'s cover:
  - a literal is `<net>` or `~<net>`, inputs in pin order;
  - a cube is its literals joined by ` & `, and each cube goes in parentheses when the cover has more than one row;
  - the cubes are joined by ` | `;
  - an off-set cover is written `~(<the same form of its rows>)`;
  - a constant is `1'b0` or `1'b1`.
- (r) **AIG size before a run** (the budget's `extra`, §7.8): the in-memory builder's upper bound.
  - `objs_est = 1 + CIs + registers + Σ_soft (cover literals + rows) + COs + register inputs`, computed in the region sweep for both exchanges.
  - `extra = ODIN3_ABC_BYTES_PER_AIG_OBJ × objs_est + 8 × objs_est` (the exchange arrays), doubled when the name map will run.
- (s) **New objects' names.** New `$lut` nodes have no name, so writers generate `$c<ID>`. A new flip-flop takes its representative's node name (§5.4). Output nets keep CO names through phase B's merges, and old internal names through phase C.
- (t) **The fake engine** (`tests/unit/abc_fake_engine.[ch]`) is test code, linked into the `test_abc_*` executables. Its `exec` understands §13.1's commands, plus the test-only `const <i>` (LUT *i* becomes constant). The knob `odin3_abc_fake_fail_on(word, fail)` makes the first command whose word 0 is `word` return `ODIN3_ERR_ENGINE` with `fail` (any kind). Every behaviour is deterministic.
- (u) **`--version`** prints a new line `linked ABC <40 hex> (adapters/abc/NOTICE)` after the existing ones when an engine is registered.
- (v) **`bench_abc`** is `tests/bench/bench_abc.c` (Release), with `--one <file.blif> [--lut K] [--via-blif DIR] [--exe ABC]`. It prints one tab-separated line per run, columns in §12's order: design, module, domain, exchange; cells, FFs, LUTs, depth; classification, AIG, checks, splice, provenance and total-outside ms; ABC ms, name-map ms and coverage %, edges, RSS before and peak MB. With `--exe`, a second line gives the pinned executable's figures on the same exchange BLIF and expanded script.
- (w) **Wrapper symbols through asm labels.** `--wrap=exit` needs a definition named `__wrap_exit`, but `bugprone-reserved-identifier` (on through `bugprone-*`) refuses C identifiers that start with `__`, and NOLINT is not allowed. So each wrapper has an `odin3_wrap_*` C name bound to its symbol with `__asm__("__wrap_<sym>")` (Task 5). `check-symbols.sh` sees the same symbols as §4.2 expects.
- (x) **Script errors carry the `abc: ` prefix.** §11 lists "AB-21's script errors" without a prefix, and the P3-1 failure text has one. Every checker error is logged as `abc: ` + AB-21's text. Task 11's cases list the text after the prefix, and the sink assertion includes it.
- (y) **Compaction after `abc`.** §6.5 has the manager compact after `abc` "using 2F-5's hook for `opt`". So `abc`'s last step is `odin3_design_compact(ctx, design)` (2F Task 13's helper in `builtin.h`). If 3D-5 merges first, Task 19 factors the `compact` pass's body into that function with the same signature, and 2F Task 13 drops its copy. 2F Task 13's IR-6 wording amendment for Peter ("… or the last step of a top-level `opt`") gains "or of `abc`".
- (z) **Choosing the exchange before the flip.** §7.1's arguments have only `--via-blif`, and the default stays BLIF until Task 21's flip. Yet `abc-diff` and H4 must run the in-memory exchange before that flip (I8). So `args.c` reads the environment variable `ODIN3_ABC_EXCHANGE` (`aig` or `blif`) once per invocation, only when `--via-blif` is absent; any other value is `abc: ODIN3_ABC_EXCHANGE must be aig or blif, not '<v>'` (`INVALID_ARG`). The flip changes only the default. The variable stays as the evidence tools' selector, documented in `abc.h` and the PHASE3 entry, not in the help line.

## Global Constraints

- **Earlier constraints.**
  - C17 with the `-Werror` flag set.
  - Functions ≤ 60 lines, CCN ≤ 15, ≤ 5 parameters, no adjacent convertible parameters (use request structs).
  - **No recursion** in `src/` or `adapters/`: region sweeps, Kahn orders, cover trees, the splice, provenance walks, depth and the checker's alias expansion use explicit stacks or worklists.
  - No VLA; no `goto` but `cleanup:`; no `NOLINT`; no `.clang-tidy` or lizard change; never `--no-verify`.
  - `odin3_` snake_case, with file-static `abc_*`/`adp_*` helpers.
  - Named constants; only `src/util` containers (side arrays from `vec`, per-run scratch from one arena); deterministic output; single-threaded core.
- **The adapter boundary (AB-5, AB-6).**
  - Only `adapters/abc/bridge.c` includes ABC headers. `bridge.c` compiles with `odin3_flags` plus `-Wno-strict-prototypes` (a source property); every other adapter file uses the full `odin3_flags`.
  - `libodin3` never links ABC.
  - The adapter exports exactly `odin3_plugin_init` and `odin3_abc_adapter_init`.
  - No `assert(` in `adapters/abc/`, and no `__builtin_trap` until Peter rules on point (a).
  - The `--wrap` wrappers never name a `__real_` symbol.
  - `third_party/abc` is never modified.
- **IR discipline (AB-18).**
  - `abc` never changes a cut cell, a constant generator or an undefined (`unconn`) net.
  - Records come before objects.
  - Any failure in run *k* leaves runs 0…*k*−1 applied and the IR valid and equivalent.
  - Nothing is compacted between runs; the manager compacts after the pass.
  - `odin3_check_design` FULL passes after every test, failing ones included.
- **Diagnostics.**
  - Every text of §11 is exact, prefixed `abc: ` (the shared budget message excepted, point (i)), lower case, names in `'…'`, no final period.
  - Script errors are located `<file>:<line>` with the file as given.
  - Every diagnostic test asserts the whole line through a capturing log sink.
  - The core never logs during an engine call (IR-17); ABC's lines arrive through the `text` callback.
- **Caps (AB-24)**, named constants in `src/passes/abc/abc.h`, lowered in tests through `odin3_abc_test_set_limits`:

  | Cap | Value |
  |---|---|
  | `ODIN3_ABC_MAX_SCRIPT_BYTES`, `_LINE_BYTES`, `_COMMANDS` | 65,536; 4,096; 1,024 |
  | `ODIN3_ABC_MAX_WORDS`, `_ALIAS_DEPTH` | 64; 8 |
  | `--lut` | 2 … `ODIN3_LUT_MAX_WIDTH` (16) |
  | `ODIN3_ABC_MAX_DOMAINS` | 64 per module |
  | `ODIN3_ABC_MAX_AIG_OBJS` | 2^26 |
  | `ODIN3_ABC_NAMEMAP_MAX_OBJS` | 2^23 |
  | `ODIN3_ABC_GATE_MAX_INPUTS` | 6 |
  | provenance edges per run | 16 × (old cells + new objects) + 65,536 |
  | `ODIN3_ABC_BYTES_PER_AIG_OBJ` | 512 until `bench_abc` calibrates it (it only goes up) |
  | design bytes | `odin3_budget_limit()` (point (i)) |
- **Determinism (AB-26).**
  - IR iteration only in ID order, and no hash-map iteration reaches the AIG, a file or the IR.
  - Two processes and two runs in one process give byte-identical `write_blif`. In the BLIF exchange `abc<r>.in.blif`, `.cmd` and `.out.blif` are identical too.
  - `.log`, `--log` and the INFO milliseconds are never compared.
- **Tests first.**
  - Unity under ASan/UBSan, with `check` around every pass.
  - An OOM sweep (`odin3_util_set_alloc_fail_after`) over every allocating entry point. Each injection gives `NO_MEMORY`, no leak, a passing FULL check, and 1E simulation equal to before.
  - Gates: `tools/build-locked --preset debug -j6 && ctest --preset debug -j4` green (before 2E-1 merges, `cmake --build --preset debug -j6`), and `tools/lint.sh` PASS.
  - `/lint` before each commit; commit at each green checkpoint (the controller pushes).
  - A read-only critique per task; a **Fable** whole-branch review before each PR.
- **Heavy runs** (standing rules; point (m)).
  - One heavy job machine-wide, in `odin3-heavy.slice` (6G, no swap), through `tools/heavy-run`, smallest first, `timeout 3h`. No tool sets `ODIN3_HEAVY_OK`; the controller does (ruling I3).
  - Implementers run only unit tests, the gate and small samples, and list in their report the sweeps they deferred.
  - `rm` only with one absolute path under `/tmp/claude-1000/`, `~/odin3-ws/work/` or `~/odin3-ws/rt-run*`.
  - Never write `~/odin3-ws/external` or `~/odin3-ws/golden`.
- **ABI.** v4, additive only. Every new public function in `odin3.h` gets its purpose/ownership/failure paragraph, and the Python cdef reads the block verbatim.
- **Python** (`tools/abc-check/`, `tests/tools/`): stdlib only, `ruff check`, `mypy --strict`, `unittest`.

## Review Focus

1. **RAM address bits left open by Parmys stay open after `abc`.** A user who runs `read_blif ch_intrinsics.parmys.blif; abc --lut 6; write_blif --vtr` expects every `single_port_ram` pin that was on `unconn` (`addr[5..14]`) to still be on `unconn`, not tied to `gnd`. A soft path from `unconn` into a RAM pin, however, ends on `gnd`, as VTR does. Pinned in Task 15 (`test_abc_region`: `bypass_unconn_hard_pin`) and Task 19 (`test_abc_unconn`, both exchanges and both engines; `abc_unconn_ch_intrinsics`: the set of hard-block pins on `unconn` is equal before and after, and so is the `--vtr` text of every `.subckt` line).
2. **Registers that start at 1 still start at 1.** For `initial q = 1` toggles, and for two init-1 registers that `scorr` merges, the user expects the mapped netlist to power up exactly as the input did. Pinned in Task 17 (`test_abc_splice`: `restore_flip_reader`, `restore_inverter_to_merge`, `restore_shared_d_not`) and Task 19 (`test_abc_init1`: `equiv-check --strict-init` passes, and every result FF has `INIT` 1 and the polarity of AB-13).
3. **A clock or a port that soft logic drives keeps its identity.** For two output ports computing the same function, and for a derived clock `clk2 = clk & en`, the user expects two distinct port nets and a clock domain that still exists after the run. Pinned in Task 17 (`test_abc_splice`: `special_port_pair_buffered`, `derived_clock_buffered`, `constant_to_clock_buffered`) and Task 19 (`abc_equiv_micro` on `tests/unit/data/abc/derived_clock.blif`).
4. **A bad script is refused before ABC starts, at its line, and nothing changes.** For a typo (`strsh`), a file command (`write_blif x.blif`), a script without a mapper (`resyn2`), or `if -K {K}` without `--lut`, the user expects one located error naming the line and an IR byte-identical to before. Pinned in Task 11 (`test_abc_script`: `refuse_unknown_command`, `refuse_file_command`, `refuse_unmapped_end`, `refuse_k_without_lut`) and Task 19 (`cli_abc_bad_script`: `write_blif` before and after are identical, and the engine's `open` is never called).
5. **Reruns are byte-identical, even with randomised commands.** For two `abc` passes in one process, two processes, and a script with `fraig`, the user expects identical netlists. Pinned in Task 6 (`test_abc_adapter`: `determinism_one_process` with `strash; fraig; if -K 6`) and Task 22 (`abc_determinism_ci`: (a) two processes and (b) A, B, then A again).

**Internal risks for the reviewers** (spec §17 items 1, 5 and 6, and the IR pieces Peter reviews):
- **Escapes and fd capture (risk 1, AB-6).** A missed `fflush` or fd restore on an escape corrupts the CLI's output. Pinned in Task 5 (each escape in its own process: fds restored, then `DISABLED`; the off-thread exit stops after its line, by the signal Peter's answer selects).
- **Splice phase B (I13).** A wrong row in its table silently merges two port nets or drops a clock. Pinned in Task 17 (every row and both columns, with simulation equal before and after).
- **Overlapping cones (risk 5).** Pinned in Task 18 (coarse mode at a lowered budget gives one record and the warning).

---

## PR 3D-0 — the spike (not merged; findings as a docs PR)

### Task 1: The spike gate (AB-17)
**Files** (outside the repo; never committed):
- `~/odin3-ws/work/spike-abc/spike.c`, its `CMakeLists.txt`, `run.sh` and `findings.md`.
- A scratch build of `third_party/abc`'s `libabc-pic` in `~/odin3-ws/work/spike-abc/build/`, configured from the submodule (read only).
- Committed later on `docs/3D-spike`: `docs/PHASE3.md`, a "3D-0 spike (AB-17)" entry (point (f)).

**Interfaces:**
- Consumes: ABC at `a3001b72` (`abcapis.h`, `Abc_FrameCreate/Destroy/Enter/Leave`, `Cmd_CommandExecute`, `Abc_FrameGiaInputMiniAig`, `Abc_FrameGiaOutputMiniLut`, `Abc_FrameGiaInputMiniLut2`, `Abc_FrameReadMiniLutNameMapping`, `Abc_NtkInputMiniAig`, `Abc_NtkOutputMiniAig`, `Cmd_CommandAdd`).
- Inputs, smallest first: `ch_intrinsics`, `stereovision3`, `diffeq2`, `diffeq1` and `mkPktMerge` (`~/odin3-ws/golden/k6_frac_N10_frac_chain_mem32K_40nm/vtr/<d>/<d>.parmys.blif`), plus the micros `common/register/register` and `operators/twobits_arithmetic_multiply/twobits_arithmetic_multiply` (or the two smallest regression goldens with a latch, when those names are absent).
  - Each input's MiniAIG comes from ABC itself: `read` with `adder`, `multiply`, `single_port_ram` and `dual_port_ram` declared `.blackbox` (the hard-model stubs of `tools/golden-blackboxes`), then `strash`, then `Abc_NtkOutputMiniAig`.
- Produces: `findings.md`, one section per criterion with its numbers, and the go/no-go for 3D-7 with the fallbacks that apply (§6.4).
- Heavy rule (finding 23): the program and the two micros run now. Every criterion on the five VTR goldens (including `dsec`, which is unbounded, and the 1,000-frame loop) runs as `ODIN3_HEAVY_OK=1 tools/heavy-run timeout 1h ~/odin3-ws/work/spike-abc/run.sh <design>`, started by the controller, once 2E-1 is on `main`.

**Tests** (the eight criteria, each a scripted assertion in `run.sh`; no heavy run):
1. `Abc_FrameGiaInputMiniAig; &put; <lib/abc/vtr.abc with K 6>; &get -m -n; Abc_FrameGiaOutputMiniLut`. Per input: the PI and PO counts equal the input's; the register count is at most the input's; every LUT has ≤ 6 fanins; and ABC's `dsec` reports the MiniLUT (written as BLIF by the spike) equivalent to the input.
2. Every result register's CI name parses as `c<n>` and maps back to an input register. This holds through:
   - the default script;
   - `scorr` on a hand-made input with two registers on equal `D`s (they merge, and the survivor keeps one of their names);
   - each of `&st`, `&synch2`, `&dch`, `&if -K 6` and `&mfs` after `&get -m -n`.
3. The name map on the aligned MiniLUT returns a literal or −1 per object. Coverage is recorded separately for the combinational and sequential inputs. Its time is ≤ 15% of the script's on each input.
4. 1,000 `Abc_FrameCreate`/`Abc_FrameDestroy` cycles: LSan reports no growth (`__lsan_do_recoverable_leak_check` before and after), the first and last MiniLUTs are byte-identical, and the per-frame cost (µs) is recorded.
5. A test command `o3spike_exit 3` (added with `Cmd_CommandAdd`, calling `exit(3)` on the session thread) returns through a `setjmp` guard. Fd 1 then writes to the original pipe.
6. Two processes, and two frames in one process, give byte-identical MiniLUTs (SHA-256 of `fanins` and `truths`). This holds for the default script and for `strash; fraig; if -K 6`, with every AB-4 generator called with `(1)` and `srand(1)` at each frame start.
7. `Abc_NtkInputMiniAig` and `Abc_FrameGiaInputMiniAig` + `&put` give identical MiniLUTs.
8. LUT count and depth on the five, with and without one observation PO per register; the difference in percent.

Acceptance:
- "Go" needs criteria 1, 2 (for the default script), 4, 6 and 7. Criterion 3 failing means cone provenance only.
- The findings PR is reviewed by Fable and merged by the controller. Peter is asked only if a fallback needs P3-9 (c) or observation outputs costing more than 1%.

## PR 3D-1 — `$lut` (first and alone)

### Task 2: The `$lut` cell, its table helpers, the JSON form and the IR text (AB-2)
**Files:**
- Create `src/ir/lut.h` (point (c)), `src/ir/cells/lut.c` and `tests/unit/test_lut_cell.c`.
- Modify `src/ir/cells/cells.h` (`#include "ir/lut.h"`; `extern const odin3_celltype_def odin3_cell_lut;`) and `src/ir/cells/builtin.c` (append `&odin3_cell_lut` after 2E's eleven cells; conflict note 2).
- Modify `tests/unit/test_ir_cells.c` (the expected table grows by one, `BUILTIN_COUNT` 74 → 75) and `tests/unit/test_ir_proc.c` (`BUILTIN_COUNT` at line 40, 74 → 75) (finding 13).
- Modify `CMakeLists.txt` (the source, `odin3_add_unit_test(test_lut_cell)`, CTest `lut_json_yosys` in the existing `ODIN3_YOSYS` block, and CTest `lut_rtlil_yosys` in the `ODIN3_YOSYS055` block).
- Modify `docs/IR.md`: §2's "LUT cells (3D)" paragraph, verbatim, after "BLIF cells (used by 1C)".
- Modify `docs/DESIGN.md` §5.2: "`$sop` (BLIF `.names` covers) and `$_LUT_K_`" becomes "`$sop` (BLIF `.names` covers) and `$lut` (a K-input LUT, Yosys semantics; `docs/IR.md` LUT cells)".

**Interfaces:**
- Consumes: `ODIN3_PORT_VEC`, `ODIN3_PORT_BIT`, `ODIN3_P_INT`, `ODIN3_P_BITS`, `odin3_cells_check_int`, `odin3_cells_check_bits`, `odin3_cells_sim_out`, `odin3_sim_cell`, `odin3_arena_alloc`.
- Produces (`src/ir/lut.h`):
  - `uint32_t odin3_lut_builtin_index(void);` is the index of `&odin3_cell_lut` in `odin3_builtin_celltypes` (ruling I5: the only way a test or a pass names `$lut`'s built-in ID).
  - `enum { ODIN3_LUT_MAX_WIDTH = 16 };`, `enum { ODIN3_LUT_P_WIDTH = 0, ODIN3_LUT_P_LUT = 1 };` and `enum { ODIN3_LUT_PORT_A = 0, ODIN3_LUT_PORT_Y = 1 };`
  - `uint32_t odin3_lut_words(uint32_t width);` gives the 64-bit words of a 2^width-bit table: 1 for width ≤ 6, else 2^(width−6).
  - `odin3_status odin3_lut_params(odin3_arena *arena, uint32_t width, const uint64_t *words, odin3_value params[2]);` builds `WIDTH` and `LUT` from a table: bit *i* of `words` (word *i*/64, bit *i*%64) is `LUT[i]`, and bits from 2^width up are ignored. `INVALID_ARG` for width 0 or above 16, or a NULL argument.
  - `bool odin3_lut_table(const odin3_value params[2], uint64_t *words, uint32_t n_words);` reads a valid `$lut`'s table, with the upper bits zero. False for parameters that `verify` rejects, or for `n_words` < `odin3_lut_words(WIDTH)`.
  - `odin3_status odin3_lut_from_engine(odin3_arena *arena, uint32_t k, uint32_t lut_size, const uint32_t *truth, odin3_value params[2]);` reads the engine layout of §2: `W = lut_size > 5 ? 1 << (lut_size − 5) : 1` 32-bit words, word 0 and bit 0 least significant, bits 0 … 2^k−1, everything above ignored. `INVALID_ARG` for k 0, k > lut_size or lut_size > 16.
- Produces (`src/ir/cells/lut.c`): `odin3_cell_lut` = `{"$lut", ODIN3_GRAN_BIT, 0, {A in vector on WIDTH, Y out scalar}, {WIDTH INT 1, LUT BITS {0,1}}, lut_verify, lut_const_value, lut_sim, NULL}`.
  - `lut_verify`: `WIDTH` through `odin3_cells_check_int` with `{"WIDTH", 1, 16}`, then `LUT` through `odin3_cells_check_bits(…, "LUT", 1u << WIDTH)`, then each byte ≤ 1. Otherwise it logs `$lut: LUT bit <i> is <x|z>; only 0 and 1 are allowed`.
  - `lut_const_value`: `ODIN3_CONST_0` when every `LUT` byte is 0, `ODIN3_CONST_1` when every byte is 1, else `ODIN3_CONST_NONE`.
  - `lut_sim`: *i* = Σ A[j]·2^j over the input bytes, then `Y = LUT[i]`.

**Tests (`test_lut_cell.c`; Unity, ASan):**
- `registry_lookup` (ruling I5): `odin3_builtin_celltypes[odin3_lut_builtin_index()] == &odin3_cell_lut`, the entry occurs exactly once, and `odin3_celltype_find(design, "$lut")` resolves to that built-in. No test names a number for it.
- `defaults_identity`: a node with the defaults has `WIDTH` 1, `LUT` bytes `{0, 1}` and 2 pins. Under `odin3_sim_build` it simulates A=0 → Y=0 and A=1 → Y=1.
- `verify_refusals`: each gives `INVALID_ARG`, an unchanged node count, and exactly two sink lines, the second the IR's `node_create: '$lut' rejects these parameters`. `WIDTH` 0 and 17 → `cell parameter WIDTH must be an int in 1..16`; `WIDTH` 2 with a 3-bit `LUT` → `cell parameter LUT must be 4 bits`; `LUT` byte 2 = x (2) → `$lut: LUT bit 2 is x; only 0 and 1 are allowed`, and z (3) → `… is z; …`.
- `const_value`: `WIDTH` 3 with all 0 → `ODIN3_CONST_0`; all 1 → `ODIN3_CONST_1`; one byte 1 → `ODIN3_CONST_NONE`.
- `simulate_tables` (`odin3_prng`, seed `0x3D1`): every table × every input for `WIDTH` 1–2; 1,000 random tables × every input for 3–6; 1,000 random tables × 64 random inputs for 7–16. `Y` always equals byte *i* of `LUT`.
- `bit_order`: `WIDTH` 2, `LUT` = `4'b1000` → Y = 1 only for A[0] = A[1] = 1. `LUT` = `4'b0010` → Y = 1 only for A[0] = 1, A[1] = 0 (A[0] is the LSB of the index).
- `table_helpers`:
  - `odin3_lut_words` of 1, 6, 7 and 16 → 1, 1, 2, 1024.
  - `odin3_lut_params(arena, 7, {0x8000000000000001, 0x1})` sets `LUT` bytes 0, 63 and 64 and no other; `odin3_lut_table` reads the same words back.
  - Width 17 → `INVALID_ARG`.
- `engine_word_layout` (I10): `lut_size` 6 (W = 2), k = 3, `truth` = `{0xFFFFFFA5, 0xFFFFFFFF}` → `LUT` = `8'b10100101`: bits 8 and up are ignored. With `lut_size` 4 (W = 1), k = 4 and `{0x0000F00F}` → bytes 0–3 and 12–15 are 1. k = 7 with `lut_size` 6 → `INVALID_ARG`.
- `json_form`: `write_json` of a module with one `$lut` (`WIDTH` 2, `LUT` = `4'b0110`, A on `a[1:0]`, Y on `y`) holds the members `"type": "$lut"`, `"LUT": "0110"`, `"WIDTH": "00000000000000000000000000000010"`, `"A": "input"` and `"Y": "output"` (MSB-first `LUT`, as Yosys).
- CTest `lut_json_yosys` (`ODIN3_YOSYS` block): `yosys -q -p "read_json F; stat"` on that file exits 0, and its output matches `\$lut +1`.
- `rtlil_form` (finding 21): `odin3_rtlil_text` of the same module holds `    parameter \WIDTH 2` and `    parameter \LUT 4'0110` (MSB first, AB-2). CTest `lut_rtlil_yosys` (`ODIN3_YOSYS055` block): Yosys 0.55 `read_rtlil F; stat` exits 0 and reports `$lut 1`.
- OOM sweep over `odin3_lut_params` and over node creation with a 16-input `$lut`.

## PR 3D-2 — the adapter and the engine ABI

### Task 3: Building ABC: `libabc-pic`, the pin, flags and the PIC `abc` tool (AB-5)
**Files:**
- Modify `CMakeLists.txt`, the `ODIN3_WITH_ABC` block (conflict note 1).
- Create `cmake/abc_pin.cmake` and `tests/tools/test_abc_build.py`.
- Modify `docs/ORACLES.md` (the `abc` tool is now `odin3_abc_exe`, built from `libabc-pic`) and `CLAUDE.md` (Layout: `adapters/{slang,abc}`; ABC's C++ stays in `third_party/`).

**Interfaces:**
- `cmake/abc_pin.cmake` sets `ODIN3_ABC_COMMIT` = `a3001b72edc5de22442e942165487fddf150e3d0` and compares it with `git -C third_party/abc rev-parse HEAD`. On a difference it stops configure with `third_party/abc is at <sha>; ORACLES.md pins <pin>`. It runs in script mode (`cmake -P`) too, reading `ODIN3_ABC_SOURCE_DIR` and the test-only `ODIN3_ABC_HEAD_OVERRIDE`.
- Inside `odin3_add_abc()`:
  - `CMAKE_C_FLAGS_DEBUG`/`CMAKE_CXX_FLAGS_DEBUG` are `-g -O2`, and `_RELEASE` is `-O2 -DNDEBUG`;
  - `set_property(TARGET libabc-pic APPEND PROPERTY COMPILE_DEFINITIONS ABC_NO_HISTORY)`;
  - neither `odin3_flags` nor the sanitizers reach ABC's targets.
- `add_executable(odin3_abc_exe third_party/abc/src/base/main/main.c)`, created **inside** `odin3_add_abc()` next to the flag overrides, so it keeps ABC's GNU C settings, not C17 (finding 19). It is linked to `libabc-pic`, with `OUTPUT_NAME abc` and `RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/third_party/abc`. `abc_tool` (ALL) depends on it. Every `ODIN3_ABC=$<TARGET_FILE:abc>` becomes `$<TARGET_FILE:odin3_abc_exe>`, so ABC compiles once per build directory.
- `odin3` (the CLI target) gets `ODIN3_HAVE_ABC` when `ODIN3_WITH_ABC` is on, and `ODIN3_ABC_COMMIT_STR="<40 hex>"` goes to the adapter (Task 5).

**Tests:**
- `test_abc_build.py` (in `python_tools`):
  - `cmake -DODIN3_ABC_SOURCE_DIR=third_party/abc -DODIN3_ABC_HEAD_OVERRIDE=0000000000000000000000000000000000000000 -P cmake/abc_pin.cmake` exits non-zero, and stderr contains `third_party/abc is at 0000000000000000000000000000000000000000; ORACLES.md pins a3001b72edc5de22442e942165487fddf150e3d0`.
  - Without the override it exits 0.
  - With `ODIN3_ABC` set, `$ODIN3_ABC -c "read t.blif; strash; print_stats"` run in a fresh temporary directory exits 0, prints `and =`, and leaves no `abc.history` (`ABC_NO_HISTORY`). `t.blif` is a two-gate fixture written by the test.
- The existing `equiv-check` and `writer-check` CTests pass unchanged on `odin3_abc_exe` (`ctest --preset debug -R 'equiv|writer_check|python_tools'`).

### Task 4: The engine ABI, `ODIN3_ERR_ENGINE` and the registry (AB-7, Q1, Q4)
**Files:**
- Modify `include/odin3/odin3.h`: before `ODIN3_STATUS_COUNT`, `ODIN3_ERR_ENGINE = 8, /* the ABC engine failed or is unavailable (3D) */`, so the count becomes 9; §4.3's engine section verbatim inside the cdef block; the paragraph of `odin3_abc_engine_register`; and the history line of point (d).
- Modify `src/api/version.c` (the status string table).
- Create `src/api/abc.c`, `src/passes/abc/engine.h`, `src/passes/abc/engine.c`, `src/passes/abc/abc_test.h` and `tests/unit/test_abc_engine_abi.c`.
- Modify `tests/unit/test_api.c` (line 36's count and the string; finding 14), `tests/tools/test_python_walk.py` (the cdef holds the engine section) and `CMakeLists.txt`.

**Interfaces:**
- Produces (`odin3.h`): §4.3 exactly: `ODIN3_ABC_ENGINE_VERSION`, `odin3_abc_fail_kind`, `odin3_abc_fail`, `odin3_abc_text_fn`, `odin3_abc_aig`, `odin3_abc_luts`, `odin3_abc_engine`, and `odin3_status odin3_abc_engine_register(const odin3_abc_engine *engine);`.
- Produces (`src/passes/abc/engine.h`):
  - `const odin3_abc_engine *odin3_abc_engine_get(void);` returns the test override, else the registered copy, else NULL.
  - `odin3_status odin3_abc_engine_set(const odin3_abc_engine *engine);` stores a copy and makes it final. `INVALID_ARG` when an engine is already set.
- Produces (`src/passes/abc/abc_test.h`): `void odin3_abc_test_set_engine(const odin3_abc_engine *engine);` (NULL clears the override). Task 11 adds the alias hook, and Task 14 adds `odin3_abc_test_set_limits`.
- `odin3_abc_engine_register` validates in this order and logs point (b)'s texts: NULL, the version, each of the six functions in table order, the four `reserved` slots, `abc_commit`, and an engine already registered.

**Tests (`test_abc_engine_abi.c`; the cases run in this order, because registration is final):**
- NULL → `INVALID_ARG`, `odin3_abc_engine_register: invalid argument engine`.
- Version 2 → `odin3_abc_engine_register: engine version 2, expected 1`.
- `exec` NULL → `odin3_abc_engine_register: engine 'fake' has no 'exec' function`.
- `reserved[2]` non-NULL → `odin3_abc_engine_register: engine 'fake': reserved[2] must be NULL`.
- `abc_commit` in upper case, and one of 39 digits → `odin3_abc_engine_register: engine 'fake': abc_commit '<s>' is not 40 lower-case hex digits`.
- A valid table → `ODIN3_OK`, and `odin3_abc_engine_get()->name` is `"fake"`. A second valid table → `INVALID_ARG`, `odin3_abc_engine_register: abc engine already registered (fake)`.
- `odin3_abc_test_set_engine(&dbl)` makes `get` return `&dbl`'s copy; NULL restores `"fake"`.
- `odin3_status_string(ODIN3_ERR_ENGINE)` is `"ODIN3_ERR_ENGINE"`, and `ODIN3_STATUS_COUNT` is 9 (`test_api`).
- `abi_exports` passes (the function is declared and exported). The Python binding loads, `lib.ODIN3_ERR_ENGINE == 8`, and `odin3_abc_engine_register` is in the cdef.
- `cli_version` still prints `(ABI 4)`.

### Task 5: Guard, wrappers, thread check, capture, session, exports and the symbol checks (AB-6, AB-8; point (a))
**Files:**
- Create in `adapters/abc/`: `guard.h`, `guard.c`, `session.h`, `session.c`, `capture.c`, `adapter.c`, `odin3_abc.h`, `exports.map` (`{ global: odin3_plugin_init; odin3_abc_adapter_init; local: *; };`), `lsan.supp`, and `README.md` (the boundary and the honest coverage of §4.2).
- Create `adapters/abc/bridge.h` and the first part of `bridge.c`: frame open and close, and `exec`.
- Modify `CMakeLists.txt`:
  - the target `odin3_abc` (SHARED, `OUTPUT_NAME odin3_abc`, `C_VISIBILITY_PRESET hidden`, `LINKER_LANGUAGE CXX`, linked PRIVATE to `libodin3` and `libabc-pic`, with §4.2's link options verbatim);
  - `bridge.c`'s source property `COMPILE_OPTIONS -Wno-strict-prototypes` and its ABC include directories as `SYSTEM`;
  - a test variant `odin3_abc_testlib` (OBJECT) of the same sources with `ODIN3_ABC_TEST_HOOKS=1`, linked into `test_abc_adapter`;
  - CTests `abc_adapter_exports` and `abc_adapter_no_assert`, and `symbol_rules` with `$<TARGET_FILE:odin3_abc>` added.
- Modify `tools/check-symbols.sh` (the new mode), `tools/check_rules.py` (the `assert(` rule), `tools/lint.sh` (`adapters/abc/*.c` in every C check), and `tests/tools/test_check_rules.py`.
- Create `tests/unit/test_abc_adapter.c`.

**Interfaces:**
- `guard.h`:
  - `typedef odin3_status (*odin3_guard_fn)(void *arg);`
  - `odin3_status odin3_guard_run(odin3_guard_fn fn, void *arg, odin3_abc_fail *fail);` runs `fn(arg)` under `setjmp`, with fds 1 and 2 captured (below). On a session-thread escape it restores the fds first, fills `*fail` with `EXIT` and the code, `ABORT`, or `ASSERT` and `"<file>:<line>: <expr>"` (≤ 255 bytes), disables the engine, and returns `ODIN3_ERR_ENGINE`.
  - `bool odin3_guard_disabled(void);`
  - Point (a)'s `ODIN3_ADAPTER_ASSERT` (the clean return only) and `odin3_status odin3_adapter_assert_fail(const char *file, int line, const char *expr);`
  - **Wrapper symbols through asm labels** (point (w)): `bugprone-reserved-identifier` refuses C identifiers that start with `__`. So each wrapper is declared, for example, `_Noreturn void odin3_wrap_exit(int code) __asm__("__wrap_exit");`, and likewise `odin3_wrap__exit` (`__wrap__exit`), `odin3_wrap_abort`, `odin3_wrap_assert_fail` (`__wrap___assert_fail(const char *expr, const char *file, unsigned int line, const char *func)`), `odin3_wrap_system`, `odin3_wrap_popen`, `odin3_wrap_fork`, `odin3_wrap_execv`, `odin3_wrap_execvp` and `odin3_wrap_signal`. The last six return −1, NULL, −1, −1, −1 and `SIG_ERR`, set `errno = EPERM`, and add `ABC tried to <call>; refused` to the session's output.
  - The escapes on another thread, or with no guard active, write `odin3: linked ABC called <exit|abort|assert> outside its session thread; stopping` to the saved original stderr fd, then call `adp_offthread_stop()`, whose body is Peter's answer (point (a)).
- `session.h`:
  - `typedef struct odin3_capture { FILE *file; int saved_out, saved_err; long read_at; } odin3_capture;`
  - `odin3_status odin3_capture_begin(odin3_capture *cap);` flushes stdout and stderr and `dup2`s fds 1 and 2 onto `cap->file`.
  - `void odin3_capture_end(odin3_capture *cap, odin3_abc_text_fn text, void *user);` flushes, restores the fds, delivers the complete new lines (a partial last line waits for the next call) and truncates the file.
  - The engine functions of the linked engine: `odin3_status odin3_abc_session_open(void *self, odin3_abc_text_fn text, void *user, odin3_abc_fail *fail);`, `odin3_status odin3_abc_session_exec(void *self, const char *command, odin3_abc_fail *fail);` and `void odin3_abc_session_close(void *self);`. Task 6 adds `load_aig`, `get_luts` and `name_map`.
- `bridge.h` (the only ABC-facing calls; ABC types stay inside `bridge.c`):
  - `odin3_status odin3_bridge_frame_open(void);` does `Abc_FrameCreate`, `Abc_FrameEnter`, batch mode, `set check`, every AB-4 generator called with `(1)`, and `srand(1)`.
  - `void odin3_bridge_frame_close(bool destroy);`
  - `int odin3_bridge_exec(const char *command);`
- `odin3_abc.h`:
  - `odin3_status odin3_abc_adapter_init(void);` registers the linked engine (`name` `"linked"`, `abc_commit` `ODIN3_ABC_COMMIT_STR`).
  - `odin3_status odin3_plugin_init(uint32_t host_abi);` returns `ODIN3_ERR_ABI_MISMATCH` for another ABI, else the first function's status.
- `check-symbols.sh --only F1,F2 --forbid S1,… LIB.so`. Exit 1, one line each:
  - `check-symbols: <lib> exports <sym>, which is not in --only`;
  - `check-symbols: <lib> does not export <sym>`;
  - `check-symbols: <lib> imports forbidden <sym>`.
  - Exit 2 on usage errors.
- `check_rules.py`: `adapters/abc/<f>:<n>: use ODIN3_ADAPTER_ASSERT, not assert() (3D §4.1)`. The existing trap rule is unchanged until Peter rules (point (a)).

**Tests:**
- `test_abc_adapter.c` (real ABC; the session cases):
  - `sessions_100`: 100 open/close cycles return `OK`. A nested `open` gives `ODIN3_ERR_ENGINE` with `fail.kind` = `BUSY`.
  - `capture_echo`: `exec "echo hello"` delivers exactly the line `hello` to the callback. During the call, fds 1 and 2 (pipes set up by the test) receive nothing; after it they are the pipes again (a write reaches the test's reader).
  - `exec_failure`: a command that ABC rejects (`strash` with no network) gives `COMMAND` and its non-zero code.
- Escapes, each in its own forked process with `ASAN_OPTIONS=detect_leaks=0`:
  - `o3test_exit 3` → `ENGINE`, `{EXIT, 3}`, fds restored; the next `open` gives `DISABLED`.
  - `o3test_assert` → `{ASSERT, 0, "<file>:<line>: <expr>"}` with a non-empty text.
  - `o3test_thread_exit` → its stderr holds `odin3: linked ABC called exit outside its session thread; stopping`, and the child dies by the signal Peter's answer selects (`SIGILL` for the trap, `SIGKILL` otherwise). This case is written once he rules.
- `adapter_assert_clean`: the hook `odin3_abc_test_invariant(false)` returns `ODIN3_ERR_ENGINE`, with `fail.kind` = `ASSERT` and a `fail.text` that starts `adapter: adapters/abc/` and ends `: o3test invariant`; the next `open` gives `DISABLED`.
- `abc_adapter_exports`: `check-symbols.sh --only odin3_plugin_init,odin3_abc_adapter_init --forbid exit,_exit,_Exit,quick_exit,abort,system,popen,fork,execv,execvp,signal <libodin3_abc.so>` exits 0. The same command on `libodin3.so` exits 1, with the "not in --only" line.
- `symbol_rules` with the adapter added is green. `abi_exports` is unchanged and green.
- `abc_adapter_no_assert`: `check_rules.py adapters/abc/*.c adapters/abc/*.h` exits 0. `test_check_rules.py` gains a fixture with `assert(x)` under `adapters/abc/` (the message), and one with `__builtin_trap()` under `adapters/abc/` (refused by the existing rule).
- `lsan.supp` suppresses only ABC functions (`Abc_*`, `Gia_*`, `Cmd_*`, `Extra_*`, `Mem_*`). The review checks that no suppression names an `odin3_`/`adp_` function.

### Task 6: Bridge calls, test hooks, CLI start-up, `--version`, NOTICE and the licence scan (AB-8, AB-9, AB-16 calls)
**Files:**
- Modify `adapters/abc/bridge.[ch]`, `session.[ch]` and `adapter.c`.
- Create `adapters/abc/NOTICE` (`copyright.txt`, plus every third-party notice found by the licence scan of `third_party/abc/src`; the scan's list goes in the PR description).
- Modify `src/cli/main.c` (conflict note 7), `tests/cli/test_cli.sh` (`cli_version_abc`, `cli_abc_unavailable_warning`), `tests/unit/test_abc_adapter.c` and `CMakeLists.txt` (`odin3` links `odin3_abc` PRIVATE under `ODIN3_WITH_ABC`; the test variant adds `-Wl,--wrap=pthread_create` with a counting wrapper).

**Interfaces:**
- `odin3_status odin3_abc_session_load_aig(void *self, const odin3_abc_aig *aig, odin3_abc_fail *fail);`
  - It checks a copy: every AND has `lit0 < lit1 < 2·index`, PIs and POs are as §4.3 lays them out, and `n_regs` ≤ the PI and PO counts. A violation is `RESULT`, with `text` naming the first bad object.
  - Then `Abc_FrameGiaInputMiniAig`, and the GIA's `vNamesIn` set to `c<n>` for PI *n*.
- `odin3_status odin3_abc_session_get_luts(void *self, odin3_abc_luts *luts, odin3_abc_fail *fail);` is `Abc_FrameGiaOutputMiniLut`, with `ci_from` parsed from `vNamesIn`: `c<n>` → *n*, anything else → −1. No mapping in the `&` space → `RESULT`, `no mapping in the & space`.
- `odin3_status odin3_abc_session_name_map(void *self, const odin3_abc_luts *aligned, const int32_t **lits, odin3_abc_fail *fail);` checks that the aligned counts equal the loaded AIG's PI, PO and register counts (else `RESULT`), then calls `Abc_FrameGiaInputMiniLut2` and `Abc_FrameReadMiniLutNameMapping`.
- Test hooks under `ODIN3_ABC_TEST_HOOKS` (`bridge.c`):
  - the commands `o3test_exit <n>`, `o3test_assert` and `o3test_thread_exit`, added with `Cmd_CommandAdd` at frame open;
  - `int odin3_abc_test_split(const char *line, char ***argv);` (ABC's `CmdSplitLine`; Task 11 uses it);
  - `odin3_status odin3_abc_test_invariant(bool ok);`
  - `void odin3_abc_test_fail_alloc_after(uint32_t n);` fails the adapter's own copies and its `tmpfile()` after *n* successes (§13.6).
  - `uint32_t odin3_abc_test_thread_count(void);` counts `pthread_create` calls.
- CLI: under `ODIN3_HAVE_ABC`, `main.c` calls `odin3_abc_adapter_init()` before any `--plugin`. A failure logs the warning `odin3: linked ABC unavailable: <status string>` and continues. `odin3 --version` adds the line `linked ABC <40 hex> (adapters/abc/NOTICE)` (point (u)).

**Tests:**
- `test_abc_adapter.c`:
  - `load_aig_toggle`: two PIs plus one register; `load_aig`, `exec "&put"`, `exec "if -K 6"`, `exec "&get -m -n"`, `get_luts` → `n_regs` 1, `lut_size` 6, `ci_from[2]` = 2 (the register's PI). Every LUT's fanins precede it.
  - `load_aig_rejects`: an AND with `lit0 > lit1` → `RESULT`, and a text naming object 3.
  - `get_luts_unmapped`: after `&put; strash` and no mapping → `RESULT`, `no mapping in the & space`.
  - `name_map_aligned`: on the same toggle, `name_map` with the aligned result gives one literal or −1 per object, with no −1 on a LUT that equals a PI's literal; mismatched counts → `RESULT`.
  - `adapter_oom`: with the alloc hook, `load_aig`'s copy and `open`'s `tmpfile()` each give `ODIN3_ERR_NO_MEMORY`, and the session stays usable.
  - `no_threads_default_script`: the default script on the 40-AND network starts 0 threads (§9).
  - `determinism_one_process` (Review Focus 5): the default script with K 6, and `strash; fraig; if -K 6`, each run twice in one process on a 40-AND network built by the test (`odin3_prng` seed `0x3D2`), give byte-identical `fanins` and `truths`.
- `cli_version_abc` matches `^linked ABC [0-9a-f]{40} \(adapters/abc/NOTICE\)$` on one line.
- `cli_abc_unavailable_warning`: a test build hook makes registration fail, and the CLI logs the warning and still runs `stats`.
- Licence: the PR lists every licence header found under `third_party/abc/src`. A licence that cannot be combined stops the PR for Peter.

## PR 3D-3 — run records: SHA-256, pass-run arguments and information

### Task 7: SHA-256 (AB-20 identity)
**Files:** Create `src/util/sha256.h`, `src/util/sha256.c` and `tests/unit/test_sha256.c`; modify `CMakeLists.txt` and `src/util/README.md` (one line).

**Interfaces:**
- `typedef struct odin3_sha256 { uint32_t state[8]; uint64_t bytes; uint8_t block[64]; uint32_t used; } odin3_sha256;`
- `void odin3_sha256_init(odin3_sha256 *ctx);`
- `void odin3_sha256_update(odin3_sha256 *ctx, const void *data, size_t len);`
- `void odin3_sha256_final(odin3_sha256 *ctx, uint8_t digest[32]);`
- `void odin3_sha256_hex(const uint8_t digest[32], char hex[65]);` writes lower case, NUL-terminated.
- No allocation, FIPS 180-4, the round constants as a named table.

**Tests (`test_sha256.c`):**
- `""` → `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`.
- `"abc"` → `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`.
- `"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"` → `248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1`.
- One million `a` → `cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0`, both in one update and streamed in chunks of 1, 63, 64, 65 and 1,000 bytes (each chunking gives the same digest).
- 64 KiB of `odin3_prng` bytes (seed `0x3D7`): one call against 7-byte chunks, equal.
- No allocation: `odin3_util_set_alloc_fail_after(0)` and the digest is unchanged.

### Task 8: Pass-run arguments and run information; IR-12 (AB-25 §8.1, §8.5)
**Files:**
- Modify `src/ir/prov.h` and `src/ir/prov.c` (the run row becomes `{name, args, info}`) and `src/passes/manager.c` (`open_run`).
- Modify `include/odin3/odin3.h` (two accessors, the history line extended; point (d)) and `src/api/design.c`.
- Modify `docs/IR.md`: the IR-12 `run` row, the paragraphs "The pass-run table" and "Provenance across ABC" verbatim from §8.5 after "Why `run` and `op`", and the §11 sentence.
- Create `tests/unit/test_passrun.c`; modify `tests/unit/test_abi_ir.c` and `CMakeLists.txt`.
- If 3E-1 has merged: `src/backends/blif/writer.c`'s `# abc:` loop and `vtr_header_abc_runs` (point (g)).

**Interfaces** (§8.1 exactly, plus one formatting helper):
- `odin3_status odin3_pass_run_begin_args(odin3_design *design, uint32_t pass_name_str, uint32_t args_str, odin3_pass_ctx *ctx);` (`args_str` 0 = none). `odin3_pass_run_begin` stays, with args 0.
- `uint32_t odin3_passrun_args(const odin3_design *design, odin3_passrun_id run);` and `uint32_t odin3_passrun_info(const odin3_design *design, odin3_passrun_id run);` (0 = none, also for an unknown run).
- `odin3_status odin3_passrun_set_info(const odin3_pass_ctx *ctx, uint32_t info_str);` `INVALID_ARG` (logged) when:
  - it is already set: `odin3_passrun_set_info: run <r> already has information`;
  - the run is 0 or unknown: `odin3_passrun_set_info: no run <r>`;
  - `info_str` is 0 or not a strtab ID: `odin3_passrun_set_info: invalid argument info_str`.
- `odin3_status odin3_passrun_info_append(odin3_strbuf *out, const char *key, odin3_bytes value);` appends `key=value`, with a separating blank when `out` is not empty. A value holding a blank, `"` or `\` is written in double quotes, with `\"` and `\\`.
- ABI:
  - `odin3_status odin3_design_get_passrun_count(const odin3_design *design, uint32_t *count);`
  - `odin3_status odin3_design_get_passrun(const odin3_design *design, uint32_t run, const char **name, const char **args, const char **info);` gives `""` for none, and `INVALID_ARG` (logged as the conventions say) for run 0 or run ≥ count.
  - Each has its paragraph. The strings live as long as the design (the strtab).
- `open_run` trims the arguments (leading and trailing blanks) and interns them. Empty arguments give 0.

**Tests (`test_passrun.c`):**
- A script `read_blif t.blif; stats   ; t_args -x  ` (with a test pass `t_args` that accepts `-x`; finding 15):
  - run 1 has args `t.blif`;
  - `stats` has args 0;
  - `t_args` has args `-x`;
  - `odin3_design_get_passrun` returns `""` for the empty args.
- A reader that opens its own run through `odin3_pass_run_begin` has args 0.
- Info:
  - `set_info` once → OK, and the ABI gives it back;
  - a second time → `INVALID_ARG` with `odin3_passrun_set_info: run 2 already has information`;
  - run 0 → `odin3_passrun_set_info: no run 0`;
  - `info_str` 0 → `invalid argument info_str`.
- `odin3_passrun_info_append`: `script` with `lib/abc/vtr.abc` → `script=lib/abc/vtr.abc`. Then `K` with `6` → `script=lib/abc/vtr.abc K=6`. A value `a b"c\d` → `x="a b\"c\\d"`.
- `compact` keeps every row (name, args and info compare equal before and after).
- ABI: `get_passrun_count` equals `odin3_passrun_end`. Run 0 and run = count → `INVALID_ARG`. `abi_exports` is green, and the Python cdef has both functions.
- `vtr_header_abc_runs` (only when 3E-1 has merged; otherwise 3E-1's rebase adds it): a design with two runs named `abc` and info `script=a.abc sha256=<64 hex> abc=<40 hex> K=6 exchange=blif depth=3` (made directly with `odin3_pass_run_begin_args(design, <"abc">, 0, &ctx)` and `odin3_passrun_set_info`, so no test pass collides with the built-in `abc`) writes, after the `# Odin III …` line, exactly two lines `# abc: script=a.abc …` in run order, and then the blank line.

## PR 3D-4 — the script checker

### Task 10: The alias table and the allow-list data (§7.4, §7.5)
**Files:** Create `src/passes/abc/alias.c`, `src/passes/abc/allow.c` and `src/passes/abc/script.h` (the types below; Task 11 adds the parser's declarations). Create `tests/unit/test_abc_alias_table.c`; modify `CMakeLists.txt` (the test gets `ODIN3_ABC_RC="${PROJECT_SOURCE_DIR}/third_party/abc/abc.rc"`).

**Interfaces (`script.h`):**
- `typedef struct odin3_abc_alias { const char *name; const char *expansion; } odin3_abc_alias;`
- `const odin3_abc_alias *odin3_abc_alias_find(const char *name);` looks up the test table when one is set, else `k_abc_aliases`, which holds every `alias` line of `third_party/abc/abc.rc` at the pin, in file order.
- `uint32_t odin3_abc_alias_count(void);` and `const odin3_abc_alias *odin3_abc_alias_at(uint32_t index);`
- `typedef enum odin3_abc_class { ODIN3_ABC_CLASS_OTHER = 0, ODIN3_ABC_CLASS_OPT, ODIN3_ABC_CLASS_MAP, ODIN3_ABC_CLASS_GIA, ODIN3_ABC_CLASS_PRINT, ODIN3_ABC_CLASS_FILES, ODIN3_ABC_CLASS_PROCESS, ODIN3_ABC_CLASS_LATCHES } odin3_abc_class;`
- `typedef enum odin3_abc_effect { ODIN3_ABC_FX_NONE = 0, ODIN3_ABC_FX_NTK_AIG, ODIN3_ABC_FX_NTK_KEEP, ODIN3_ABC_FX_NTK_MAPPED, ODIN3_ABC_FX_GIA_GET, ODIN3_ABC_FX_NTK_PUT, ODIN3_ABC_FX_GIA_AIG, ODIN3_ABC_FX_GIA_MAPPED, ODIN3_ABC_FX_GIA_KEEP } odin3_abc_effect;` (§7.5's "Effect" column, in row order)
- `typedef struct odin3_abc_command_info { const char *name; uint8_t cls; uint8_t effect; const char *file_opts; } odin3_abc_command_info;`, where `file_opts` lists the option letters whose value is a file or a string (`""` for none).
- `const odin3_abc_command_info *odin3_abc_command_find(const char *name);` returns the allowed row, a refused row (class `FILES`, `PROCESS` or `LATCHES`), or NULL for "other".
- `void odin3_abc_test_set_aliases(const odin3_abc_alias *table, uint32_t n);` (in `abc_test.h`; NULL restores)

The rows are §7.5's, and each command name is verified at the pin.
- The `print_*` set is the 32 `print_*` commands at `a3001b72` except `print_constr` and `print_status`, plus `&ps`, `time` and `echo`.
- The refused rows are §7.5's "among others", plus every refused alias's stopping command (§7.4).
- `file_opts` is read from each command's usage text and `Extra_UtilGetopt` handling in `base/abci/abc.c`. The commit message cites the line of each non-empty entry.

**Tests (`test_abc_alias_table.c`):**
- `matches_abc_rc`: parsing `$ODIN3_ABC_RC`'s `alias` lines (`alias <name> "<expansion>"` or unquoted) gives exactly `k_abc_aliases`, in order, with equal expansions. A pin bump that changes `abc.rc` fails here.
- §7.4's lists:
  - every "usable" one-word alias expands to its listed command (`b` → `balance`, `rwz` → `rewrite -z`, `sif` → `if -s`, …);
  - every refused one-word alias's command has class `FILES`, `PROCESS`, `LATCHES` or `OTHER`;
  - every usable script alias's expansion equals §7.4's table;
  - each refused script alias contains its stopping command.
- `allow_rows`: §7.5's spaces and effects, row by row:
  - `strash` → `OPT`/`NTK_AIG`; `sweep` → `OPT`/`NTK_KEEP`; `if` → `MAP`/`NTK_MAPPED`; `mfs2` → `MAP`/`NTK_KEEP`;
  - `&get` → `GIA`/`GIA_GET`; `&put` → `GIA`/`NTK_PUT`; `&dch` → `GIA`/`GIA_AIG`; `&if` → `GIA`/`GIA_MAPPED`; `&mfs` → `GIA`/`GIA_KEEP`;
  - `print_stats` → `PRINT`/`NONE`;
  - `read_blif` → `FILES`; `quit` → `PROCESS`; `retime` → `LATCHES`;
  - `print_constr` and `print_status` → not allowed; `frobnicate` → NULL.
- `alias_depth_in_table`: no real alias expands more than 3 levels.

### Task 11: The checker, the embedded default script, the fuzzer and the adapter checks (AB-21, AB-23; point (p))
**Files:**
- Create `src/passes/abc/script.c`, `lib/abc/vtr.abc` (§7.7 verbatim), `cmake/abc_default_script.c.in` and `src/passes/abc/abc.h` (its caps part).
- CMake generates `${PROJECT_BINARY_DIR}/generated/abc/default_script.c` with `file(READ lib/abc/vtr.abc HEX)`.
- Create `tests/unit/test_abc_script.c`, `tests/unit/test_abc_default_script.c`, `tests/fuzz/fuzz_abc_script.c` and `tests/fuzz/abc_script_seeds/`: VTR's script, one file per alias, and one per refused example of this task's tests.
- Modify `tests/unit/test_abc_adapter.c` (point (p)) and `CMakeLists.txt`:
  - `fuzz_abc_script` and the 60 s `fuzz_abc_script_smoke` in the `ODIN3_FUZZ` block;
  - `fuzz_abc_script_long` through `odin3_add_sweep_test`.

**Interfaces:**
- `abc.h` (caps, point (m)):
  - `enum { ODIN3_ABC_MAX_SCRIPT_BYTES = 65536, ODIN3_ABC_MAX_LINE_BYTES = 4096, ODIN3_ABC_MAX_COMMANDS = 1024, ODIN3_ABC_MAX_WORDS = 64, ODIN3_ABC_MAX_ALIAS_DEPTH = 8, ODIN3_ABC_LUT_MIN = 2 };`
  - `enum odin3_abc_exchange { ODIN3_ABC_EXCHANGE_AIG = 0, ODIN3_ABC_EXCHANGE_BLIF = 1 };`
- `script.h`:
  - `typedef struct odin3_abc_script_src { const char *given; const char *resolved; odin3_bytes text; } odin3_abc_script_src;` `given` is the path as given, or `builtin:lib/abc/vtr.abc`. `resolved` is the path read when `text.ptr` is NULL. `text` holds the bytes when already in memory.
  - `typedef enum odin3_abc_space { ODIN3_ABC_SPACE_NTK = 0, ODIN3_ABC_SPACE_GIA = 1 } odin3_abc_space;`
  - `typedef struct odin3_abc_cmd { const char *line; uint32_t src_line; const char *alias; } odin3_abc_cmd;` `line` holds the expanded, NUL-terminated words joined by one blank. `alias` is the outermost alias, or NULL.
  - `typedef struct odin3_abc_script { const odin3_abc_cmd *cmds; uint32_t n_cmds; bool uses_k; uint8_t final_space; char sha256_hex[65]; odin3_arena *arena; } odin3_abc_script;`
  - `odin3_status odin3_abc_script_parse(const odin3_abc_script_src *src, uint32_t lut_k, uint32_t exchange, odin3_abc_script *out);` takes `lut_k` 0 for no `--lut`. It gives `ODIN3_OK`, or `PARSE` after exactly one located error, or `IO` (`abc: <resolved>: cannot read …: <strerror>`), or `NO_MEMORY`. `out` is untouched on failure.
  - `void odin3_abc_script_free(odin3_abc_script *script);`
  - `const odin3_abc_cmd *odin3_abc_script_cmd(const odin3_abc_script *script, uint32_t index);` (NULL past the end)
- Default script:
  - `extern const unsigned char odin3_abc_default_script[];`, `extern const uint32_t odin3_abc_default_script_len;`
  - `#define ODIN3_ABC_DEFAULT_SCRIPT_NAME "builtin:lib/abc/vtr.abc"`
- The algorithm is AB-21's twelve steps, in order, in one pass over the bytes and then over the expanded commands. Every message is `<given>:<line>: …` except the size and unmapped messages, which have no line. The SHA-256 is of the bytes read, before any expansion.

**Tests (`test_abc_script.c`; every text exact, each at its line, and logged as `abc: ` + the text, point (x)):**
- **Bytes.**
  - 65,537 bytes → `t.abc: abc script is larger than 65536 bytes`.
  - A NUL on line 3 → `t.abc:3: abc script contains a NUL byte`.
  - `strash\x01` on line 2 → `t.abc:2: byte 0x01 is not allowed outside a comment`. The same byte after `#` is accepted.
  - A 4,097-byte line → `t.abc:1: line longer than 4096 bytes`.
  - CRLF line ends give the same commands as LF.
- **Quotes and shell.** `read "x"` → `t.abc:1: quotes are not allowed in abc scripts`. `!ls` → `t.abc:1: '!' (shell escape) is not allowed in abc scripts`.
- **`{K}`.**
  - `if -K {K}` with `lut_k` 6 → the line `if -K 6`.
  - With `lut_k` 0 → `t.abc:1: the script uses {K} but abc was given no --lut` (`refuse_k_without_lut`, Review Focus 4).
  - `if -K {N}` → `t.abc:1: unknown substitution '{N}' (only {K} is defined)`.
- **Words.** 65 words → `t.abc:1: command has more than 64 words`.
- **Aliases.**
  - `b -l` → `balance -l` with alias `b`.
  - `resyn -v` → the six commands of `resyn` expanded. Only the last gets `-v`, so its line is `balance -v`, and every command has alias `resyn` and line 1.
  - With `odin3_abc_test_set_aliases` and a 9-deep chain → `t.abc:1: alias 'a0' expands more than 8 levels deep`.
- **Allow-list (Review Focus 4).**
  - `strsh` → `t.abc:1: abc command 'strsh' is not an allowed ABC command (3D spec §7.5)` (`refuse_unknown_command`).
  - `write_blif x.blif` → `t.abc:1: abc command 'write_blif' is not allowed: abc scripts cannot read or write files` (`refuse_file_command`).
  - `quit` → `… cannot control the ABC process`.
  - `retime` → `… it retimes or changes initial values (PHASE3 #12)`.
  - `r x.blif` → `t.abc:1: abc command 'read' (from alias 'r') is not allowed: abc scripts cannot read or write files`.
- **Arguments.**
  - `if -K six` → `t.abc:1: argument 'six' of 'if' is neither an option nor a number`.
  - `echo hello-world` is accepted.
  - For **every** row of the allow-list and every letter `x` in its `file_opts`, `<cmd> -<x> 6` → `t.abc:1: option '-<x>' of '<cmd>' takes a file or a string and is not allowed` (table-driven; I12).
- **Count.** 1,025 `b` commands → `t.abc:1025: script expands to more than 1024 commands`. 1,024 are accepted, then `if -K 6` makes 1,025 and is refused.
- **Spaces and mapping.**
  - `strash` alone → `t.abc: the script leaves the network unmapped; end it with a LUT mapper such as 'if -K {K}'` (`refuse_unmapped_end`). So do the empty script and `resyn2`.
  - BLIF: `&if -K 6` first → `t.abc:1: '&if' needs a network in the & space, which the BLIF exchange starts empty; add '&get'`.
  - In-memory: the same script is accepted, with final space GIA.
  - `strash; if -K 6; mfs2` → final space NTK.
  - `&get -n; &if -K 6` (BLIF) → final space GIA.
- **Output.** Every expanded line contains none of `"`, `'`, `!`, `;`, `#`, `{`, `}`. The SHA-256 of a two-line script equals `odin3_sha256` of its bytes.
- **Caps at their limits**, through `odin3_abc_test_set_limits` (Task 14 adds the setter; until then the tests use the constants directly).
- OOM sweep over `parse`.

**Tests (`test_abc_default_script.c`):**
- The embedded bytes equal `lib/abc/vtr.abc` (read through `ODIN3_SOURCE_DIR`), and so do their SHA-256s.
- With `lut_k` 6 the commands are exactly `strash`, `ifraig -v`, `scorr -v`, `dc2 -v`, `dch -f`, `if -K 6 -v`, `mfs2 -v`, `print_stats`, with source lines 5–12.
- With `lut_k` 0 → `builtin:lib/abc/vtr.abc:10: the script uses {K} but abc was given no --lut`.

**Tests (adapter, point (p); `test_abc_adapter.c`; they land with whichever of 3D-2 and 3D-4 merges second):**
- `checked_is_what_runs`: for every accepted line in `test_abc_script.c`, every alias expansion and every seed of `tests/fuzz/abc_script_seeds/`, `odin3_abc_test_split(line)` gives exactly the checker's words.
- `no_files` (I12): a temporary directory holds only a file `6` with 64 bytes of garbage, plus an existing `abc.history` with known bytes. Every allowed command runs with its default options on a 20-AND network (`load_aig`; `&put`; then each command, with `if -K 6` first wherever a command needs a mapped network). Afterwards the directory holds exactly `6` and `abc.history`, both byte-identical, and the `6` was never opened: its content would make any reader fail with an error the session would log, and the log has no such line.

**Fuzzer (`fuzz_abc_script.c`):** input = one K byte (`K = 2 + byte % 15`, or no `--lut` when the byte is 0), then the script bytes.
- No crash and no leak.
- When accepted, these properties hold, each checked by `__builtin_trap()` in `tests/` only:
  - every word 0 has an allow-list row;
  - every argument is an option or a number;
  - n ≤ 1,024;
  - the final space is mapped;
  - no command holds a quote, `!`, `;`, `#`, `{`, `}`, an alias name as word 0, or a file option.
- When rejected, exactly one line was logged.
- `fuzz_abc_script_smoke` runs 60 s with no crash.

## PR 3D-6 — `abc_script` in projects and `--abc-script` on the command line

### Task 12: The record, the `.o3proj` key, the request, the dump, the reference parser and the fixtures (AB-27 §10.1–§10.3)
**Files:**
- Modify `src/frontends/project/project.h` (`odin3_proj_path abc_script;` after `flow`; `odin3_project_request` gains `const char *abc_script;`), `o3proj.c` (`key_abc_script`, and the key-table row `{"abc_script", 1, 1, "abc_script <file>", key_abc_script}` after `flow`), `project.c` (the request applied beside `top`) and `dump.c` (`"abc_script"` right after `"flow"`).
- Modify `src/passes/project_pass.c`: `read_project` puts `odin3_pass_get_options().abc_script` into the request (Task 13 adds the field; until then the request's field is NULL).
- Modify `tools/project-fixtures/project_fixtures.py` (the same key, error and dump rules).
- Modify every positive `tests/golden/projects/*/expected.json`: `"abc_script": null` after `"flow"`, one mechanical commit (Sonnet `medium`), checked by `project-fixtures check`.
- Create `tests/golden/projects/{o3proj_abc_script,neg_abc_script_twice,neg_abc_script_missing,neg_abc_script_flist}/`. Modify `tests/golden/projects/README.md` (four matrix rows; the key row `abc_script`: the ABC script path or null, stored, which `abc` uses, 3D §7.2; and the key order `… overrides flow abc_script output ignored duplicates`).
- Modify `docs/DESIGN.md` §4.0 (§10.1 items 1–4, verbatim).
- Modify `tests/unit/test_project_o3proj.c`, `test_project_record.c` and `tests/tools/test_project_fixtures.py`.

**Interfaces:**
- `key_abc_script` mirrors `key_flow`:
  - a second use is `syntax`, `abc_script given twice`, naming `abc_script`;
  - `arg_path(…, ODIN3_PROJ_WANT_FILE, …)` makes a missing file `missing_file` at its line.
- `odin3_project_read` with `req->abc_script` non-NULL:
  - The path must be a regular file, else an unlocated `missing_file` with names `[<as given>]`.
  - It replaces the record's `abc_script`. `given` is the CLI word, and `at` is `{0}`.
  - When the resolved paths differ, it logs the info line `<entry>: --abc-script <new> replaces the project abc_script <old>` after finish, like `--top`'s.
- Dump: `"abc_script": "<resolved>"` or `null`.

**Tests:**
- `test_project_o3proj.c`:
  - `abc_script abc/map.abc` → record `given` `abc/map.abc`, resolved against the project directory, `at` = line 3;
  - twice → `p.o3proj:4:1: error: syntax: abc_script given twice` (the 2B located form), names `["abc_script"]`;
  - a missing file → `missing_file` at its line, names `["abc/none.abc"]`.
- `test_project_record.c`: the request `abc_script` `cli.abc` (an existing file) over a project naming `abc/map.abc` → the record holds `cli.abc`, and the info line `p.o3proj: --abc-script cli.abc replaces the project abc_script abc/map.abc`. A request naming a missing file → `missing_file`, and the record is dropped.
- The corpus (`project-fixtures check` and the `project_corpus` CTest):
  - `o3proj_abc_script` dumps `"abc_script": "abc/map.abc"`, and its `ref.cmd`/`ref.blif` are as in `o3proj_arch_rules` (the script is stored, not applied);
  - `neg_abc_script_twice` → `syntax` at the second line, names `["abc_script"]`;
  - `neg_abc_script_missing` → `missing_file`, names `["abc/none.abc"]`;
  - `neg_abc_script_flist` (`src/top.v` then `-abc_script x.abc`) → `unknown_option` at `files.f:2`, names `["-abc_script"]`.
- Every positive `expected.json` has the key, and the reference parser agrees on every case.
- A Quartus `.qsf` with `set_global_assignment -name ABC_SCRIPT x.abc` lists it under `ignored` like any other assignment, and sets no `abc_script`.

### Task 13: `--abc-script`, the pass option and `odin3_pass_set_abc_script` (AB-27 §10.2; AB-20 step 3)
**Files:** Modify `src/passes/manager.h` (`odin3_pass_options` gains `const char *abc_script;`), `src/passes/manager.c`, `include/odin3/odin3.h` (the function, its paragraph, the history line; point (d)), `src/api/pass.c`, `src/cli/main.c` (conflict note 7), `tests/cli/test_cli.sh`, `tests/unit/test_pass_manager.c` and `tests/unit/test_abi_ext.c`.

**Interfaces:**
- `odin3_status odin3_pass_set_abc_script(const char *path);` has `odin3_pass_set_top_name`'s contract: NULL clears it; the string is copied and owned by the library; `NO_MEMORY` keeps the old value.
- CLI:
  - `--abc-script FILE` joins `k_value_options` and `cli_state`. At scan time a path that is not a regular file is `odin3: --abc-script FILE: no such file` (`bad_value`, exit 2).
  - `main.c` calls `odin3_pass_set_abc_script` before the script runs.
  - The usage line is `  --abc-script FILE the ABC script of every abc pass (below abc --script)`.

**Tests:**
- `test_abi_ext.c`: set, then `odin3_pass_get_options().abc_script` equals the path; NULL clears it; `odin3_pass_set_abc_script` is in the cdef, and `abi_exports` is green.
- `test_cli.sh`:
  - `cli_abc_script_missing`: `odin3 --abc-script nope.abc -p stats` → exit 2, stderr `odin3: --abc-script nope.abc: no such file`;
  - `cli_abc_script_project`: `odin3 --abc-script cli.abc --project p.o3proj -p dump_project` → the dump's `"abc_script"` is `cli.abc`'s resolved path, and the replacement info line is on stderr;
  - `--help` shows the usage line.

## PR 3D-5 — the `abc` pass on the BLIF exchange (M1 with 3E and 3F)

### Task 9: `$lut` in the Verilog writer (§2, point (q))
**In PR 3D-5** (finding 12), which already waits for 3E-5 (`odin3_blif_isop`).

**Files:** Modify `src/backends/verilog/writer.c` (`BUILTINS` gains `{"$lut", FORM_LUT, NULL, 0}`, and `write_lut`). Create `tests/unit/test_lut_verilog.c`. Modify `CMakeLists.txt` (the unit test, and CTest `lut_verilog_icarus` when `ODIN3_IVERILOG` and `ODIN3_VVP` are found).

**Interfaces:**
- Consumes: `odin3_lut_table` (Task 2) and `odin3_blif_isop(const uint64_t *tt, uint32_t k, odin3_strbuf *cover, uint32_t *rows, int *constant)` (3E-5).
- Produces: `write_lut` writes `assign <Y> = <expr>;` in point (q)'s form, with input names in pin order A[0] … A[k−1].

**Tests (`test_lut_verilog.c`):**
- AND (`WIDTH` 2, `LUT` `4'b1000`, A = {a, b}) → `assign y = a & b;`.
- NOT (`WIDTH` 1, `LUT` `2'b01`) → `assign y = ~a;` (on-set wins the tie).
- XOR (`4'b0110`) → two cubes, each in parentheses, joined by ` | `. The test asserts the line matches `^  assign y = \((~?a) & (~?b)\) \| \((~?a) & (~?b)\);$`, and that evaluating the cubes gives the table.
- OR3 (`8'b11111110`; finding 16): the off-set has one row against the on-set's three, so → `assign y = ~(~a & ~b & ~c);`.
- A constant `WIDTH` 2 all-ones `$lut` → `assign y = 1'b1;`.
- CTest `lut_verilog_icarus`: the unit test writes `build/<preset>/lut/luts.v` (one module, 16 random `$lut`s per width 1–6, `odin3_prng` seed `0x3D9`) and `luts_tb.v`. The testbench drives all 2^k inputs of each LUT and compares with its table. `iverilog` + `vvp` print `PASS` and exit 0.

### Task 14: Arguments, script precedence, start-up checks, the budget and the pass row (AB-19, AB-20, AB-24, AB-28)
**Files:**
- Create `src/passes/abc/args.c`, `src/passes/abc/run.h`, `src/passes/abc/run.c` and `tests/unit/test_abc_args.c`.
- Create `tests/unit/abc_fake_engine.h` and `.c`: `open`, `close` and an `exec` that records lines. Task 16 completes it.
- Extend `src/passes/abc/abc.h` (options, the pass) and `abc_test.h` (limits).
- Modify `src/passes/builtin.c`/`builtin.h` (conflict note 5), `src/passes/budget.[ch]` (point (i): the cgroup bound, the one message, `odin3_budget_test_set_cgroup_root`), `tests/unit/test_budget.c` (`budget_e6`, `check_and_message`, `limit_default` and a new `limit_cgroup`), `tests/unit/test_proc_pass.c` (2E's budget text), 2F's `budget`/`budget_opt` tests if 2F has merged, `docs/PASSES.md` (the row, verbatim from §7.1) and `CMakeLists.txt`.

**Interfaces:**
- `abc.h`:
  - `typedef enum odin3_abc_prov_mode { ODIN3_ABC_PROV_FINE = 0, ODIN3_ABC_PROV_CONE = 1 } odin3_abc_prov_mode;`
  - `typedef struct odin3_abc_opts { const char *script; uint32_t lut_k; const char *via_blif; const char *log; uint8_t prov; } odin3_abc_opts;` (NULL or 0 = not given)
  - `odin3_status odin3_abc_parse_args(odin3_bytes args, odin3_arena *arena, odin3_abc_opts *out);` splits with `odin3_pass_arg_next`. Each option may appear once.
  - `typedef enum odin3_abc_script_source { ODIN3_ABC_SRC_PASS = 1, ODIN3_ABC_SRC_PROJECT, ODIN3_ABC_SRC_CLI, ODIN3_ABC_SRC_BUILTIN } odin3_abc_script_source;`
  - `typedef struct odin3_abc_script_choice { const char *given; const char *resolved; uint8_t source; } odin3_abc_script_choice;`
  - `odin3_status odin3_abc_choose_script(const odin3_design *design, const odin3_abc_opts *opts, odin3_arena *arena, odin3_abc_script_choice *out);` applies AB-20 in order:
    1. `--script`, resolved against the working directory;
    2. the project record's `abc_script` (Task 12);
    3. `odin3_pass_get_options().abc_script` (Task 13);
    4. `ODIN3_ABC_DEFAULT_SCRIPT_NAME`, with resolved `embedded`.
  - `odin3_status odin3_abc_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args, void *user);` and `extern const odin3_pass_def odin3_pass_abc;` (point (n)). The help is §7.1's line exactly.
- `run.h`:
  - `typedef struct odin3_abc_run odin3_abc_run;` is one invocation: the context, options, checked script, engine, exchange, run counter *r*, log file, arena, the per-type gate-table cache, and the current module, domain and region.
  - `odin3_status odin3_abc_run_create(odin3_pass_ctx *ctx, const odin3_abc_opts *opts, odin3_abc_run **out);` and `void odin3_abc_run_destroy(odin3_abc_run *run);`
  - `odin3_status odin3_abc_check_views(const odin3_design *design);` gives the first word cell in module creation order, then node-ID order: `abc: module '<m>' is not in the netlist view: <type> '<cell>'; run lower first` (`ODIN3_ERR_CHECK`).
- Start-up order, before any IR change or engine call:
  1. arguments;
  2. the `--via-blif` path bytes (I12);
  3. the script choice and `odin3_abc_script_parse`;
  4. the engine (`odin3_abc_engine_get() == NULL` → `abc: linked ABC is not available (no engine registered; build with ODIN3_WITH_ABC=ON)`, `ENGINE`);
  5. the views;
  6. `--log` opened (truncated);
  7. the first INFO line, `abc: script <given> (<resolved>), sha256 <first 16 hex>…, K=<K|none>, exchange <aig|blif>, ABC <first 12 hex of abc_commit>`.
- `abc_test.h`: `typedef struct odin3_abc_limits { uint32_t max_domains, max_aig_objs, namemap_max_objs, max_script_bytes, max_line_bytes, max_commands, max_words, max_alias_depth; uint64_t prov_edge_budget; } odin3_abc_limits;` and `void odin3_abc_test_set_limits(const odin3_abc_limits *limits);` (NULL restores; 0 fields keep the default; each value is clamped to its default).
- `budget.c` (point (i)): `odin3_budget_limit()` = min(60% of physical memory, the smallest numeric `memory.max` on the `/proc/self/cgroup` path's ancestors). The reader takes test paths through `odin3_budget_test_set_cgroup_root(const char *root, const char *self_cgroup)`.
- Budget use: before each run, `odin3_budget_check(ctx, design, extra)` with point (r)'s `extra`. Over it is `abc needs more than <N> bytes (the design's budget is <B> bytes)` (`PARSE`), before the engine opens.

**Tests (`test_abc_args.c`, with the fake engine through `odin3_abc_test_set_engine`):**
- Arguments, each exact, each `INVALID_ARG`, and the IR unchanged (`write_blif` text before and after is identical):
  - `--frob` → `abc: unknown argument '--frob'`;
  - `--lut 6 --lut 4` → `abc: --lut given twice`;
  - `--script` → `abc: --script needs a value`;
  - `--lut 1`, `--lut 17` and `--lut x` → `abc: --lut must be an integer from 2 to 16, not '1'` (and so on);
  - `--prov fast` → `abc: --prov must be fine or cone, not 'fast'`;
  - `--via-blif "a;b"`, a path holding a tab, and one holding `'` → `abc: --via-blif directory '<dir>' contains a control character, a quote, ';' or '#'; ABC cannot read it`.
- `IO`:
  - `--script nope.abc` → `abc: nope.abc: cannot read the script: No such file or directory`;
  - `--log /nonexistent/x` → `abc: /nonexistent/x: cannot open the log: No such file or directory`;
  - `--via-blif` under a read-only directory → `abc: <dir>: cannot create the directory: Permission denied`.
- **Precedence** (`odin3_abc_choose_script`):
  - nothing → source `BUILTIN`, given `builtin:lib/abc/vtr.abc`;
  - a project with `abc_script abc/map.abc` → `PROJECT`, given `abc/map.abc`;
  - plus the option `cli.abc` set → `PROJECT` with the record replaced by `read_project` (given `cli.abc`); without a project → `CLI`;
  - plus `--script s.abc` → `PASS`, given `s.abc`.
- **No engine** → `ENGINE` and the "not available" line, with no IR change.
- **Views:** a module holding an `$add` → `abc: module 'm' is not in the netlist view: $add '$c3'; run lower first` (`CHECK`).
- **INFO line:** with the default script and `--lut 6` it matches `^abc: script builtin:lib/abc/vtr.abc \(embedded\), sha256 [0-9a-f]{16}…, K=6, exchange blif, ABC [0-9a-f]{12}$`, and the 16 hex digits are the default script's SHA-256 prefix.
- **Script refused** (`cli_abc_bad_script`'s unit half; Review Focus 4): `--script bad.abc` with line 1 `write_blif x.blif` → `PARSE`, the one line `abc: bad.abc:1: abc command 'write_blif' is not allowed: abc scripts cannot read or write files` (point (x)), the fake engine's `open` count 0, and `write_blif` before equal to after.
- **Budget:**
  - `odin3_budget_test_set_limit(4096)` → `abc needs more than <N> bytes (the design's budget is 4096 bytes: the smaller of 60% of physical memory and the cgroup's memory.max)`, `PARSE`, `open` count 0.
  - `limit_cgroup` (`test_budget.c`): a fake root where the scope's `memory.max` is `max` and the parent slice's is `6442450944` → `odin3_budget_limit()` ≤ 6442450944. `limit_default` with an empty fake root → exactly 60% of physical memory. The 2E/2F text assertions read the new message.
- The pass is in `odin3_pass_count()`'s list with `wants_ast` false and `processes` `REFUSE`. A design with a `$proc` → `pass 'abc': module 'm' has processes; run proc first`.

### Task 15: The region: soft, register, constant and cut cells; CIs, COs, domains and loops (AB-10, AB-11, AB-12)
**Files:** Create `src/passes/abc/region.h`, `src/passes/abc/region.c` and `tests/unit/test_abc_region.c`; modify `run.c` (the per-module domain loop) and `CMakeLists.txt`.

**Interfaces:**
- `typedef enum odin3_abc_role { ODIN3_ABC_ROLE_CUT = 0, ODIN3_ABC_ROLE_SOFT, ODIN3_ABC_ROLE_REG, ODIN3_ABC_ROLE_CONST } odin3_abc_role;`
- `typedef enum odin3_abc_domain_kind { ODIN3_ABC_DOM_NONE = 0, ODIN3_ABC_DOM_EDGE, ODIN3_ABC_DOM_GLOBAL } odin3_abc_domain_kind;`
- `typedef struct odin3_abc_domain { uint8_t kind; bool negedge; odin3_net_id clock; odin3_vec ffs; } odin3_abc_domain;` (`ffs` holds node IDs in ascending order: a set of flip-flops, I13)
- `odin3_status odin3_abc_domains(odin3_abc_run *run, odin3_module *module, odin3_vec *domains);` computes the domains once per module, sorted per §5.3 with point (k)'s name bytes. More than the cap → `abc: module '<m>' has <N> clock domains; at most 64` (`PARSE`, nothing changed).
- `odin3_status odin3_abc_gate_table(odin3_abc_run *run, odin3_celltype_id type, uint64_t *table, uint32_t *n_inputs);` runs the 1E hook once per type over all 2^n inputs (input *j* = the *j*-th input pin), and caches the result.
- `typedef struct odin3_abc_region { odin3_vec soft, regs, cis, cos, reg_d; uint32_t zero_pins; uint64_t objs_est; uint32_t cut_counts[5]; } odin3_abc_region;`
  - `soft` holds node IDs in Kahn order, ties by node ID;
  - `regs` holds node IDs in ascending order;
  - `cis` and `cos` hold net IDs in ascending order; `reg_d` holds each register's `D` net.
  - The cut counts are hard or black-box cells, instances, level latches, tri-state or bus cells, and other cells.
- `odin3_status odin3_abc_region_build(odin3_abc_run *run, const odin3_abc_domain *domain, odin3_abc_region *out);` is one O(nodes + pins) sweep and Kahn's algorithm. On a stall it cuts the lowest-ID remaining cell's output net (a CO and a CI) with the WARN `abc: module '<m>': combinational loop through <type> '<cell>'; its output <net> is a cut point`.
- `int odin3_abc_pin_literal(const odin3_abc_run *run, odin3_net_id net);` gives 1 for value 1; 0 for 0, x, z and undefined; −1 for no constant.
- `bool odin3_abc_net_undefined(const odin3_module *module, odin3_net_id net);` is true for a net named `unconn` driven by a zero-input, zero-row `$sop`.
- The INFO lines `abc: module '<m>': cut points: <h> hard or black-box cells, <i> instances, <l> level latches, <t> tri-state or bus cells, <o> other cells` (when any count is non-zero) and `abc: module '<m>': <N> soft-logic pins read x, z or unconn as 0` (N > 0) are logged once per module.

**Tests (`test_abc_region.c`):**
- **Every registry type:** for each entry of `odin3_builtin_celltypes`, the role follows §5.1's rule. `$sop`, `$lut` and `$_BUF_ $_NOT_ $_AND_ $_OR_ $_XOR_ $_NAND_ $_NOR_ $_XNOR_ $_MUX_` are soft. `$_CONST0_/1/X/Z_` are constant. `$_DFF_P_` in the run's domain is a register, and in another domain it is cut. `$_DLATCH_*` and the port cells are cut. A word cell gives the views error.
- **Rule edges:**
  - a test bit type with a parameter and a simulate hook → cut;
  - a 7-input test gate type → cut;
  - a `$_AND_` with `keep` = 1 → cut;
  - a `$_BUF_` whose output net has a second driver (a bus) → cut, and that net is a CI to soft readers.
- **Constants per pin (C2):** `$_CONSTX_` into a `$_AND_` → literal 0; the undefined `unconn` → 0; `$_CONST1_` → 1. The info line reads `abc: module 'm': 3 soft-logic pins read x, z or unconn as 0`.
- **Bypass** (`bypass_unconn_hard_pin`; Review Focus 1): a `single_port_ram` (from `read_techlib lib/vtr.o3lib`) with `addr[5]` on `unconn` and `addr[6]` on a `$_CONSTX_` net. Neither net is a CO, and no generator cell is in `soft`.
- **CIs and COs:**
  - an undriven net read by a soft cell → a CI;
  - a soft net read by a `$port_out` → a CO;
  - read by a flip-flop's `C` → a CO (a derived clock keeps its net);
  - a `keep` net → a CO, and still in its soft readers' cones;
  - a soft net that nothing reads → in neither list;
  - order: ascending net ID.
- **Domains:** `$_DFF_P_` on `clk_b`, `$_DFF_P_` and `$_DFF_N_` on `clk_a`, and `$_FF_` → order `clock 'clk_a' posedge`, `clock 'clk_a' negedge`, `clock 'clk_b' posedge`, `global clock`, each with its FF set. Lowered cap 2 → `abc: module 'm' has 4 clock domains; at most 2`.
- **Loops:** `n1 = ~n2; n2 = ~n1` (nodes 4 and 5) → WARN `abc: module 'm': combinational loop through $_NOT_ '$c4'; its output n1 is a cut point`, and `n1` is in both `cis` and `cos`.
- **Cut-point line:** a module with one `multiply`, one instance, one `$_DLATCH_P_` and one `keep` cell → `abc: module 'm': cut points: 1 hard or black-box cells, 1 instances, 1 level latches, 0 tri-state or bus cells, 1 other cells`.
- **Scale:** a 1,000,000-gate chain classifies and orders with no recursion, in under 2 s (Release).
- OOM sweep over `region_build`.

### Task 16: The fake engine, `abc_result` and the BLIF exchange (AB-14, AB-15, AB-13 encoding)
**Files:**
- Create `src/passes/abc/result.[ch]`, `src/passes/abc/xblif.[ch]`, `tests/unit/test_abc_result.c` and `tests/unit/test_abc_xblif.c`.
- Complete `tests/unit/abc_fake_engine.[ch]` (point (t)).
- Modify `run.c` (the BLIF session of AB-22) and `CMakeLists.txt`.

**Interfaces:**
- `result.h`:
  - `typedef struct odin3_abc_result_expect { uint32_t n_cis, n_cos, n_regs, lut_k_max; } odin3_abc_result_expect;`
  - `typedef struct odin3_abc_result { uint32_t n_cis, n_regs_out, n_luts, n_cos; odin3_vec fanin_start, fanins, lut_k, truth_start, truth, reg_from, co_driver, old_lit; } odin3_abc_result;`
    - Objects: 0 and 1 are the constants, then the `n_cis` CIs, then the `n_regs_out` register outputs, then the LUTs in topological order.
    - `truth` holds `uint64_t` words, `odin3_lut_words(k)` per LUT.
    - `reg_from` gives each result register its input register *k\**.
    - `co_driver` covers the real COs, then the observation outputs, then the register inputs.
    - `old_lit` is empty without a name map.
  - `odin3_status odin3_abc_result_check(const odin3_abc_result *res, const odin3_abc_result_expect *expect, odin3_abc_fail *fail);` fills `fail->text` with §11's `<why>`, exact: `N primary inputs, expected M`; `N outputs, expected M`; `N registers, more than the M it was given`; `register J carries no input register's name`; `a LUT with N inputs (at most 16)`; `a LUT with N inputs exceeds --lut K`. The caller logs `abc: ABC returned a malformed network: <why>`.
  - `odin3_status odin3_abc_result_fold(odin3_abc_result *res);` turns a LUT whose table is all 0 or all 1 over its 2^k bits into object 0 or 1. Its readers are rewired, and each reader LUT gets that variable's cofactor (I10).
- `xblif.h`:
  - `odin3_status odin3_abc_xblif_write(const odin3_abc_run *run, const odin3_abc_region *region, const char *path);` writes §6.2's input, with the names of its table. The cells are in Kahn order: a `$sop` keeps its rows, and gates and `$lut`s write `odin3_blif_isop`'s cover. The AB-13 inverters and `.latch`es come after the cells, and a CO that is a register's IR value gets its `1 1` buffer.
  - `odin3_status odin3_abc_xblif_commands(const odin3_abc_run *run, const char *in_abs, const char *out_abs, odin3_strbuf *out);` gives `read "<in>"`, the script's lines, `&put` when the final space is GIA, and `write_hie "<in>" "<out>"`, one per line (this is `abc<r>.cmd`).
  - `odin3_status odin3_abc_xblif_read(odin3_abc_run *run, const char *path, odin3_abc_result *out);` reads with 1C's reader into a scratch design (`odin3_design_create`, then destroyed).
    - The inputs and outputs must be exactly the names sent, in any order. Otherwise: `an unknown output '<name>'`, or the count texts.
    - A latch is identified by its output name `o3q<k>`. A `.subckt` → `a .subckt`. A latch init other than 0 → `a latch with init <v> (all were sent at 0)`.
    - Each `$sop` becomes a table. More than 16 inputs → `a LUT with N inputs (at most 16)`. A one-input `1 1` `.names` becomes a connection.
    - Kahn's algorithm, ties by node ID, gives the order.
- Fake engine (`abc_fake_engine.h`):
  - `const odin3_abc_engine *odin3_abc_fake_engine(void);`, `void odin3_abc_fake_reset(void);` and `uint32_t odin3_abc_fake_open_count(void);`
  - `const char *odin3_abc_fake_last_commands(void);`
  - It keeps one internal AIG. BLIF mode: `read "<in>"` builds the AIG from the BLIF (1C reader; each `$sop` row an AND tree; `.latch` a register, keeping its name); `write_hie` writes the network as BLIF, mapped LUTs as `.names` through `odin3_blif_isop`, registers as `.latch o3d… o3q… 0` under their names. In-memory mode (Task 20) uses `load_aig` and `get_luts`.
  - The commands: `&put`, `&get -m -n` and `strash` are no-ops. `if -K k` maps each AND to a 2-input LUT, with inverter LUTs for complemented outputs. `scorr` merges registers with equal `D` (keeping the lowest's name) and replaces a register whose `D` is its own `Q` or 0 by a constant. `const <i>` makes LUT *i* constant. Anything else is a no-op.
  - `void odin3_abc_fake_fail_on(const char *word, odin3_abc_fail fail);` (NULL `word` clears it; point (t)).

**Tests:**
- `test_abc_xblif.c`, fixture `t1`: `$port_in` a, b and clk; `n = $_AND_(a, b)`; `$_DFF_P_ q` (C = clk, D = n, `INIT` 1); `$port_out y ← q`. `abc0.in.blif` is exactly:

  ```
  .model o3abc
  .inputs o3ci0 o3ci1
  .outputs o3co0
  .names o3ci0 o3ci1 o3n<n>
  11 1
  .names o3n<n> o3d0
  0 1
  .latch o3d0 o3q0 0
  .names o3q0 o3r0
  0 1
  .names o3r0 o3co0
  1 1
  .end
  ```

  `<n>` is the net ID of `n`, read by the test. `clk` is not a CI (a clock pin is not a data pin).
- The same fixture with `INIT` 0 has no `o3d`/`o3r` inverters, and the CO buffer reads `o3q0`. With `INIT` 2 or 3 the latch is sent at 0, with no inverters.
- `abc0.cmd` with the default script and `--lut 6` is exactly `read "<abs in>"`, the eight commands of Task 11, then `write_hie "<abs in>" "<abs out>"`.
- Read-back, each crafted `.out.blif` giving `ENGINE` and the exact line `abc: ABC returned a malformed network: <why>` with: `an unknown output 'o3co9'`; `a .subckt`; `a latch with init 1 (all were sent at 0)`; `a LUT with 17 inputs (at most 16)`; `a LUT with 5 inputs exceeds --lut 4`; `2 registers, more than the 1 it was given`; `register 0 carries no input register's name` (a latch output `foo`); `3 primary inputs, expected 2`; `2 outputs, expected 1`.
- A one-input `1 1` `.names` becomes a connection, not a LUT.
- Determinism: two writes of `t1` give identical `.in.blif` and `.cmd` bytes.
- `test_abc_result.c`:
  - folding: a 2-input LUT with an all-1 table read by an AND LUT → the reader becomes its cofactor (a 1-input LUT, the identity), and the constant object is 1;
  - a CO driven by an all-0 LUT → driver object 0;
  - the check texts above on hand-made results;
  - the cost is linear (a 100,000-LUT chain folds with no recursion).
- OOM sweep over `xblif_write`, `xblif_read` and `result_fold`.

### Task 17: The splice: phases A–D, special nets and the init-1 restore (AB-18, AB-13 restore)
**Files:** Create `src/passes/abc/splice.h`, `src/passes/abc/splice.c` and `tests/unit/test_abc_splice.c`; modify `run.c` and `CMakeLists.txt`. Until Task 18, every new object carries one `DERIVED` record whose parents are all the run's old cells and flip-flops, in node-ID order (the coarse form of §8.3).

**Interfaces:**
- `typedef struct odin3_abc_splice_stats { uint32_t luts, ffs, buffers, merges, deleted_cells, deleted_nets, deleted_wires; } odin3_abc_splice_stats;`
- `odin3_status odin3_abc_splice(odin3_abc_run *run, const odin3_abc_region *region, odin3_abc_result *res, odin3_abc_splice_stats *stats);` runs:
  - **Phase A:** fold, records, the restore flips, the fresh nets, the `$lut`s (through `odin3_lut_params`), the buffer `$lut`s (`WIDTH` 1, `LUT` `2'b10`) and the new flip-flops (`C` from the representative's `C` pin; `INIT` 2 when `INIT(k*)` is 2 or 3, else `INIT(k*)`; `k*`'s node name). `NO_MEMORY` deletes what phase A made, in reverse order.
  - **Phase B:** §6.5's table exactly. A special CO is a clock net, a port net, or a net with `keep`.
  - **Phase C:** delete the old cells, merge old internal nets by their uncomplemented anchors (Task 18), and merge each `k*`'s old `Q` into the new one.
  - **Phase D:** worklist cleanup.
- `odin3_status odin3_abc_canonical_const(odin3_abc_run *run, int value, odin3_net_id *out);` gives the lowest-ID net driven by `$_CONST<v>_` or by a constant `$sop` that is not undefined. When there is none, it creates a `$_CONST<v>_` node and its net.
- `odin3_status odin3_abc_restore(odin3_abc_run *run, odin3_abc_result *res);` handles every result register *j* whose `INIT(k*)` = 1:
  - it flips the table of each new LUT that reads *j*, in that variable;
  - a CO reading *j* directly gets a NOT `$lut`, and a CO reading *j* through an inverter LUT becomes a merge onto *j*'s new net (the inverter cancels; finding 17);
  - a `D` source LUT that feeds only *j* is complemented, and any other `D` source gets a NOT `$lut`;
  - a constant `D` flips.

**Tests (`test_abc_splice.c`; fake engine; each case also checks that 1E simulation, sequential, 256 vectors × 3 seeds, is equal before and after, and that `odin3_check_design` FULL passes):**
- **Phase B, every row and both columns:**
  - `fresh_net`: the CO keeps its net, now driven by a `$lut`.
  - `shared_driver`: ordinary COs `c1` and `c2` with one new driver → one net holding both names.
  - `special_port_pair_buffered` (Review Focus 3): two `$port_out` nets with the same function → two distinct nets; `c2` is driven by a buffer `$lut` from `c1`.
  - `ci_ordinary`: a CO that computes CI `d` → `merge(keep = d, drop = c)`.
  - `ci_special`: a port CO equal to a CI → a buffer `$lut` from the CI.
  - `const_ordinary`: a CO that folds to 1 → merged into the canonical constant net; with none present, a new `$_CONST1_` node.
  - `const_port`: a port CO that folds to 0 → merged into the constant-0 net (3E §9.2 writes the buffer).
  - `constant_to_clock_buffered`: a clock CO that folds to a constant → a buffer `$lut` from the constant net; the clock net survives.
  - `derived_clock_buffered` (Review Focus 3): `clk2 = clk & en` clocks a second FF set. After the pass `clk2`'s net still exists, and its domain ran second (two INFO run lines, the second `clock 'clk2' posedge`). Run 2 sees run 1's new flip-flops as cut cells: they are in no domain set (I13).
- **Flip-flops (3E §14.1 item 3):**
  - the new flip-flop is `$_DFF_P_`, with `C` on the representative's original `C` net and the node name of `k*`;
  - `INIT` 0 → 0, 2 → 2, 3 → 2;
  - `k*`'s old `Q` net is merged into the new one, so every alias name stays.
- **Folding (3E §4.2 item 1):** `const 0` (the fake) on a LUT read by a `$port_out` → no `$lut` with constant tables exists, and the port net is on the `$_CONST0_` net.
- **Restore (C1; Review Focus 2):**
  - `restore_flip_reader`: an `INIT` 1 register read by a 2-input LUT → that LUT's table is flipped in variable 0 or 1 as wired, the FF has `INIT` 1, and simulation is equal from reset.
  - `restore_inverter_to_merge`: a `$port_out` reading ¬*j* through an inverter LUT → a merge, with no NOT left.
  - `restore_shared_d_not`: the `D` source LUT also feeds a port → *j*'s `D` gets a NOT `$lut`.
  - `restore_d_only`: a `D` source that feeds only *j* → its table is complemented, with no new cell.
  - `restore_const_d`: a constant `D` → it flips.
- **Names:** COs keep their names. The old `Q` names survive as aliases.
- **Cleanup:** a new LUT nothing reads is removed; non-port nets left with no pins, members or aliases are removed; so are wires whose nets have no pins.
- **Failures:**
  - `NO_MEMORY` at every allocation of phase A → `write_blif` is byte-identical to before;
  - at every allocation of phases B–C → valid IR, simulation-equal;
  - a failure in run 2 of 2 leaves run 1 applied.

### Task 18: Provenance, depth and the run information (AB-25; points (h), (k))
**Files:** Create `src/passes/abc/prov.h`, `src/passes/abc/prov.c`, `src/passes/abc/depth.c` and `tests/unit/test_abc_prov.c`; modify `splice.c` (phase A takes the records from here), `run.c` (the run information and the final INFO) and `CMakeLists.txt`.

**Interfaces:**
- `typedef enum odin3_abc_anchor_kind { ODIN3_ABC_ANCHOR_CO = 1, ODIN3_ABC_ANCHOR_REG_IN, ODIN3_ABC_ANCHOR_REG, ODIN3_ABC_ANCHOR_CI, ODIN3_ABC_ANCHOR_NAMEMAP } odin3_abc_anchor_kind;` (§8.3's order: COs, register inputs, registers, objects)
- `typedef struct odin3_abc_anchor { uint32_t obj; odin3_net_id old_net; uint8_t kind; bool complemented; } odin3_abc_anchor;`
- `odin3_status odin3_abc_anchors(odin3_abc_run *run, const odin3_abc_result *res, odin3_vec *out);` produces the identity anchors (every CI, CO and register input, C3) and the register anchors (AB-13). Task 21 adds the name-map anchors.
- `odin3_status odin3_abc_records(odin3_abc_run *run, const odin3_abc_result *res, const odin3_vec *anchors, odin3_vec *obj_prov);` follows §8.5's rules exactly, with iterative walks, per-anchor visit stamps and one `odin3_prov_begin_op` + `odin3_prov_derive` per anchor.
  - The parents are an old region's cells in node-ID order. A multi-region LUT's extra record has the regions' records as parents, in anchor order (point (h)).
  - The edges are summed before any record is made. Over the budget, the run makes one record with the WARN `abc: module '<m>', <domain>: provenance needs N parent edges (budget M); the run's LUTs share one record`.
- `odin3_status odin3_abc_depth(const odin3_design *design, uint32_t *depth);` gives the longest path of `$lut`s in the top module between its CIs and COs (ports, flip-flops, cut cells), by Kahn order. It is 0 without a top.
- The run information (point (d) of §8.1), set once per invocation after the last run with `odin3_passrun_info_append`: `script=<given> sha256=<64 hex> abc=<engine abc_commit> K=<K|none> exchange=<aig|blif> depth=<d>`.
- The INFO lines `abc: module '<m>', <domain>: <c> cells and <f> flip-flops -> <l> LUTs (at most <k> inputs, depth <d>) and <g> flip-flops, <ms> ms in ABC` (per run) and `abc: final: depth <d>` (once, last).

**Tests (`test_abc_prov.c`):**
- **Cone mode (`--prov cone`):**
  - every new LUT's record is `DERIVED` and reaches, through `odin3_prov_sources`, the `SOURCE` records of the old cells in its CO's cone;
  - **a register's `D` cone has the old `D` cone's records as parents** (C3);
  - parents come in node-ID order.
- **Several regions:** a LUT shared by two COs carries an extra record whose two parents are the two anchors' records, CO order.
- **Forward and backward:** `odin3_prov_sources` of the new FF gives its old FF's `SOURCE`. The children of an old cell's record (the forward walk) include the new LUT.
- **First location (point (h)):** the first leaf of the backward walk of a LUT is the lowest-ID old cell's `SOURCE`.
- **Coarse mode:** `prov_edge_budget` = 1 → one record for the run, every new object carries it, and the WARN text is exact (`N`, `M` from the fixture).
- **Dead logic:** an old soft cell that reaches no CO and no register input dies without children.
- **Depth:**
  - a chain port → LUT → LUT → LUT → FF → 3;
  - a `multiply` in the middle cuts the path;
  - `abc: final: depth 3` is the last INFO line;
  - with no top → `depth=0`.
- **Run information:** with the fake engine registered as `abc_commit` `a3001b72edc5de22442e942165487fddf150e3d0`, the default script and `--lut 6`, `odin3_passrun_info` is exactly `script=builtin:lib/abc/vtr.abc sha256=<the SHA-256 of lib/abc/vtr.abc> abc=a3001b72edc5de22442e942165487fddf150e3d0 K=6 exchange=blif depth=<d>`. With `--script "my dir/s.abc"` → `script="my dir/s.abc"`.
- **The per-run INFO line** matches `^abc: module 'm', clock 'clk' posedge: 5 cells and 2 flip-flops -> 3 LUTs \(at most 2 inputs, depth 2\) and 2 flip-flops, [0-9.]+ ms in ABC$`.
- OOM sweep over `records`.

### Task 19: Real-engine integration, `equiv-check --blackbox`, `abc-equiv`, the splice fuzzer and DESIGN §6 (AB-30 §13.3–§13.4, §13.6–§13.7; 3E §14.1)
**Files:**
- Create `tests/unit/test_abc_unconn.c`, `tests/unit/test_abc_init1.c`, `tests/unit/test_abc_pass.c`, `tests/unit/data/abc/{toggle_init1,scorr_merge_init1,derived_clock,ram_unconn,two_ports,loop,keep_net}.blif`, `tests/fuzz/fuzz_abc_splice.c`, `tools/abc-check/abc-equiv`, `tools/abc-check/abc_equiv.py`, `tests/tools/test_abc_equiv.py` and `tests/abc/equiv_expect_fail.txt`.
- Modify `tools/equiv-check/equiv_check.py` (`--blackbox`, its docstring) and `tests/tools/test_equiv_check.py`.
- Modify `CMakeLists.txt`:
  - gate CTests `abc_equiv_micro`, `cli_abc`, `cli_abc_bad_script`, `cli_abc_script_precedence` and `abc_unconn_ch_intrinsics`;
  - `fuzz_abc_splice` and `fuzz_abc_splice_smoke` (2,000 runs) in the `ODIN3_FUZZ` block;
  - the sweeps `abc_equiv_golden` and `fuzz_abc_splice_long` through `odin3_add_sweep_test` (point (m)).
- Modify `pyproject.toml` and `tests/tools/__init__.py` (`tools/abc-check`), `docs/DESIGN.md` (§6 step 10, §10.1 item 5 verbatim) and `tests/cli/test_cli.sh`.

**Interfaces:**
- `equiv-check --blackbox M[,M…] [existing options] a.blif b.blif`:
  - instances of the named models are paired across the files by `.cname` when both have one, else by occurrence order per model;
  - their outputs become shared primary inputs, and their inputs compared outputs;
  - unequal counts → exit 2 with `equiv-check: --blackbox: model '<m>' has <x> instances in <a> and <y> in <b>`;
  - several clocks stay excluded (`multiclock`, exit 2, as today).
- `abc-equiv --odin3 O --equiv-check E [--step abc|equiv|all] [--lut K] [--via-blif DIR | --exchange aig|blif] [--vtr] [--keep DIR] [--expect-fail F] [--timeout S] <file or dir>…`:
  - It collects `.blif` files and sorts them all together by size, smallest first.
  - Per design it runs `O -p "read_techlib lib/vtr.o3lib; read_blif X; write_blif before.blif; abc --lut K [--via-blif D]; write_blif after.blif"` (and `write_blif --vtr after.vtr.blif` with `--vtr`). `--exchange` sets `ODIN3_ABC_EXCHANGE` for that child (point (z)).
  - Then `E --blackbox adder,multiply,single_port_ram,dual_port_ram --timeout S before.blif after.blif`.
  - Then it checks that the sets of `(model, occurrence, formal)` on `unconn` are equal in `before.blif` and `after.blif` (and in `after.vtr.blif`).
  - It prints one line per design, `<design> equivalent|differs|multiclock|timeout|error|unconn-changed <s> s`, and a summary. A listed entry (`<path> <reason>`) passes; an unlisted failure or a listed pass fails.
  - `--step` (ruling I9; default `all`): `abc` runs the `odin3` step only and leaves `before.blif`, `after.blif` (and `after.vtr.blif`) in `--keep DIR`, printing `<design> mapped <s> s`; `equiv` runs `equiv-check` and the `unconn` check on the files already in `--keep DIR` (a missing file is `error`). `--step abc|equiv` without `--keep` is a usage error (exit 2).
  - A listed `views` entry passes only when `odin3` fails with the views error (point (j)).
  - Exit 0, 1 or 2. It passes the caller's environment through unchanged; on a path under `~/odin3-ws/golden` it first calls `qor_heavy.require_heavy()` (point (m)). Children run as ordinary subprocesses of the heavy scope (no nested `--scope`).

**Tests:**
- `test_abc_unconn.c` (3E §14.1 items 2 and 4; Review Focus 1), fake and real engines, BLIF exchange (Task 20 adds the in-memory one):
  - `ram_unconn.blif`'s `single_port_ram` keeps `addr[5..14]` on the `unconn` net and `addr[4]` on its `$_CONSTX_` net (hand-built): the same net IDs, the generators alive.
  - A `$lut` reading `unconn` → after `abc`, no `.names` or `.latch` pin of the `write_blif` text is on `unconn`.
  - `unconn → $_BUF_ → addr[3]` → `addr[3]`'s net is the canonical constant-0 net (like VTR).
- `test_abc_init1.c` (C1; Review Focus 2), real engine, default script, `--lut 6`, BLIF exchange:
  - `toggle_init1.blif` → one `$_DFF_P_` with `INIT` 1;
  - `scorr_merge_init1.blif` (two `INIT` 1 latches on equal `D`) → one `$_DFF_P_` with `INIT` 1;
  - `tools/equiv-check --strict-init before after` exits 0 on both.
- `test_abc_pass.c`:
  - the fake identity in BLIF mode on every `tests/unit/data/abc/*.blif` and `tests/golden/blif/*.blif` not in `tests/abc/equiv_expect_fail.txt` → simulation-equal, and `pow.parmys.blif` → the views error `abc: module 'pow' is not in the netlist view: $pow '<cell>'; run lower first` (ruling I8);
  - the real engine with `--script` `strash; if -K 6` → `equiv-check` exit 0, and a second `abc` on the result is still equivalent;
  - an OOM sweep over the whole pass on `t1`, `derived_clock` and `toggle_init1` → `NO_MEMORY`, no leak, FULL check, simulation-equal;
  - after a pass, the module has no deleted slots (point (y)), and without `--via-blif` no `odin3-abc-*` directory is left under the test's `TMPDIR` (point (l));
  - `$lut` round trip (§13.3 item 1, 3E-5): `write_blif`, `read_blif`, `write_blif` give identical text, and the tables are equal;
  - engine failures through `odin3_abc_fake_fail_on`, each exact: `balance` failing under `resyn2; if -K {K}` → `abc: s.abc:1: command 'balance' failed (from alias 'resyn2')`; `&put` → `abc: internal command '&put' failed`; `EXIT` 3 during `strash` → `abc: ABC called exit(3) during 'strash'; linked ABC is disabled until odin3 restarts`, and likewise `ABC aborted during 'strash'; …` and `ABC assertion failed during 'strash': <text>; …`; `DISABLED` → `abc: linked ABC is disabled by an earlier failure in this process`; `BUSY` → `abc: linked ABC is busy (another session is open)`. Each is `ENGINE`, and the IR stays valid and equivalent;
  - with the real engine and the default script, `print_stats`' output reaches the log at INFO as `abc| <line>`, and `strash`'s at DEBUG.
- `abc_equiv_micro` (gate; point (j)): `abc-equiv --lut 6 --expect-fail tests/abc/equiv_expect_fail.txt` on `tests/golden/blif/*.blif` and `tests/unit/data/abc/*.blif` → every unlisted design `equivalent`, `pow.parmys.blif` failing as listed (`views`), exit 0.
- `cli_abc`: `odin3 -p "read_blif t.blif; abc --lut 4; stats"` → exit 0, `stats` lists `$lut`, and the first INFO line is as Task 14's regex.
- `cli_abc_bad_script` (Review Focus 4): `odin3 -p "read_blif t.blif; abc --script bad.abc --lut 4"` → exit 1, stderr holds `abc: bad.abc:1: abc command 'write_blif' is not allowed: abc scripts cannot read or write files`, and no `x.blif` exists.
- `cli_abc_script_precedence` (moved here, point (e)): `abc` on a two-gate BLIF with four scripts that differ only in a comment. For every combination of `--script`, `--abc-script` and a project `abc_script`, the logged hash prefix is that of the script AB-20 picks. With `--abc-script` and a project, the replacement info line appears.
- `abc_unconn_ch_intrinsics` (gate; exit 77 when the golden or 3E-1's `--vtr` is missing): `read_techlib lib/vtr.o3lib; read_blif <ch_intrinsics.parmys.blif>; abc --lut 6; write_blif --vtr W` → every `.subckt single_port_ram` line of `W` has `addr[5]=unconn` … `addr[14]=unconn`, and the set equals the golden's.
- `test_equiv_check.py`: two netlists with a `multiply` box that differ only in soft logic → 0; a change in what feeds a box input → 1; counts 3 against 2 → 2 and the exact text.
- `test_abc_equiv.py` (fakes for `odin3` and `equiv-check`): one size order across two directories; the argv of both children exactly; an `unconn` set change → `unconn-changed`; the expect-list rules (a `views` entry passes only on the views error); the summary line; `--step abc` runs no `equiv-check` and keeps the files, then `--step equiv` on the same `--keep` runs no `odin3`; `--step equiv` without `--keep` → exit 2; the child environment equals the caller's (no `ODIN3_HEAVY_OK` added); a golden path outside `heavy-run` → exit 2 with `require_heavy`'s message.
- `fuzz_abc_splice` (fake engine): bytes become a hand-built module (inputs, gates, FFs with `INIT` 0–3, `unconn` readers, a derived clock) plus K and the fake choices (`scorr`, `const <i>`).
  - Invariants (`__builtin_trap()` in `tests/` only): the status is `OK`, or an injected `NO_MEMORY`; the FULL check passes; 64 vectors of 1E simulation are equal; every new object's record reaches a `SOURCE`.
  - `fuzz_abc_splice_smoke`: 2,000 runs, no crash.
- **Deferred to the controller** (point (m)): `abc_equiv_golden` (every k6 Parmys golden except `mcml` and `LU32PEEng`, smallest first; `stereovision3` listed as `multiclock`). The run log's summary is quoted in the PR.

## PR 3D-7 — the in-memory exchange (after the spike's go)

### Task 20: The AIG builder and the in-memory protocol (AB-16, AB-22)
**Files:**
- Create `src/passes/abc/aig.h`, `src/passes/abc/aig.c` and `tests/unit/test_abc_aig.c`.
- Modify `result.c` (`odin3_abc_result_from_luts`) and `run.c` (the in-memory session; `--via-blif` still selects BLIF).
- Modify `tests/unit/abc_fake_engine.c` (in-memory mode), `test_abc_unconn.c`, `test_abc_init1.c` and `test_abc_splice.c` (every case also runs with `exchange=aig`), and `CMakeLists.txt`.

**Interfaces:**
- `typedef struct odin3_abc_aigb { odin3_vec objs, owner; odin3_u64map *hash; odin3_u64map *net_lit; uint32_t n_pis, n_regs, n_pos; } odin3_abc_aigb;` holds the MiniAIG `int32_t` pairs, the owning node per AND, and the literal per soft output net.
- `odin3_status odin3_abc_aig_build(odin3_abc_run *run, const odin3_abc_region *region, odin3_abc_aigb *out);`:
  - objects in §6.3's order: object 0; one PI per CI, then per register output; the ANDs in topological order; then the POs (COs, then register inputs);
  - literals: an inverted register's `Q` and `D` are complemented (AB-13); constants per AB-11's per-pin rule;
  - covers from `odin3_blif_isop` (gates, `$lut`s) or a `$sop`'s rows; cubes and covers as balanced pairwise trees, left to right, level by level;
  - hashing applies a·0 = 0, a·1 = a, a·a = a and a·¬a = 0, keyed `lit0 << 32 | lit1`, and the first owner wins;
  - over `ODIN3_ABC_MAX_AIG_OBJS` → `abc: module '<m>', <domain>: the AIG needs more than <cap> objects` (`PARSE`, nothing changed).
- `odin3_abc_aig odin3_abc_aig_view(const odin3_abc_aigb *aig);`, `int32_t odin3_abc_aig_lit(const odin3_abc_aigb *aig, odin3_net_id net);` (−1 none) and `void odin3_abc_aig_free(odin3_abc_aigb *aig);`
- `odin3_status odin3_abc_result_from_luts(const odin3_abc_luts *luts, const odin3_abc_result_expect *expect, odin3_arena *arena, odin3_abc_result *out);` takes the tables through `odin3_lut_from_engine`'s layout. The register correspondence comes from `ci_from` (I11). A register without a name gives `register J carries no input register's name`.
- The session: `open`; `load_aig`; `&put`; the script; `&get -m -n` when the final space is NTK; `get_luts`; `close`. A failing internal command is `abc: internal command '<cmd>' failed`.
- Until the flip, the in-memory exchange runs when `ODIN3_ABC_EXCHANGE=aig` (point (z)); the tests set it.
- Observation outputs (one PO per register) are built only if 3D-0's findings require them (criterion 2 failing for the default script and criterion 8 ≤ 1%). That change is then made in this task, with Peter told.

**Tests (`test_abc_aig.c`):**
- **Layout:** for `t1` with `INIT` 0, `objs` is exactly `[0x7FFFFFFF,0x7FFFFFFF]` × 3 PIs (a, b, the register output) after object 0, then one AND `(2, 4)`, then PO `(<q lit>, 0x7FFFFFFF)` and the register input `(6, 0x7FFFFFFF)`, with `n_regs` 1. With `INIT` 1, the register-output literal read by the PO and the register-input literal are both complemented.
- **Hashing:** `a & 0` → literal 0, no AND; `a & 1` → `a`; `a & a` → `a`; `a & ~a` → 0; two cells computing `a & b` share one AND, whose owner is the lower-ID cell.
- **Balance:** a 4-literal cube gives ANDs `(l0,l1)`, `(l2,l3)` and then their AND.
- **Equal to the IR:** on every `tests/unit/data/abc/*.blif`, a test evaluator of the AIG (sequential, registers at their sent init) equals 1E simulation of the region, over 256 vectors × 3 seeds.
- **PI names:** the fake engine's `get_luts` gives `ci_from[i]` = the loaded PI index for every register.
- **Cap:** `max_aig_objs` = 10 → the exact `PARSE` line with `10`.
- **The in-memory variants** of `test_abc_unconn`, `test_abc_init1` (real engine) and every `test_abc_splice` case pass, with `exchange=aig` in the run information.
- OOM sweep over `aig_build` and `result_from_luts`.

### Task 21: The aligned MiniLUT, the name map, fine provenance, `abc-diff` and the default flip (AB-16, AB-25 §8.2 item 3; I8)
**Files:**
- Modify `src/passes/abc/prov.c` (name-map anchors), `run.c` (the aligned MiniLUT and `name_map`), `args.c` (default exchange `aig`) and `tests/unit/test_abc_prov.c` (fine mode).
- Create `tools/abc-check/abc-diff`, `tools/abc-check/abc_diff.py` and `tests/tools/test_abc_diff.py`.
- Modify `CMakeLists.txt` (the sweeps `abc_equiv_golden_aig` and `abc_diff_golden`) and `docs/PHASE3.md` (the per-design ratios, recorded before the flip commit).

**Interfaces:**
- `odin3_status odin3_abc_aligned_build(const odin3_abc_run *run, const odin3_abc_luts *res, odin3_arena *arena, odin3_abc_luts *aligned);`:
  - the PIs as sent, one register output per input register *k*;
  - the new LUTs, with each fanin from result register *j* remapped to *k\**(*j*);
  - the real outputs as in the result;
  - register inputs: *j*'s `D` driver for *k* = *k\**(*j*), and constant 0 for any other *k*.
- Name-map anchors: each LUT whose aligned object maps to literal ℓ is paired with the lowest-ID old net *n* with `lit(n)` = ℓ or ¬ℓ. A complemented anchor counts for provenance and never moves names.
- The name map is skipped above `ODIN3_ABC_NAMEMAP_MAX_OBJS`, under `--prov cone`, and on the BLIF exchange. A `RESULT` or `COMMAND` failure in it falls back to cones. Each case logs `abc: module '<m>', <domain>: no name map (<reason>); provenance uses cut cones`, with the reason `--prov cone`, `BLIF exchange`, `the AIG has more than 8388608 objects` or `ABC: <fail text>`.
- `abc-diff --odin3 O --equiv-check E [--lut K] [--timeout S] [--expect-fail F] <file or dir>…`, smallest first:
  - per design, both exchanges (`--via-blif` in a temporary directory under `~/odin3-ws/work/abc-diff/`, and `ODIN3_ABC_EXCHANGE=aig`, point (z));
  - each result `equiv-check --blackbox`-equivalent to the input and to the other;
  - one line `<design> luts <b> <a> <a/b> ffs <b> <a> <a/b> depth <b> <a> <a/b> <verdict>` (ratios to 3 decimals), where `explain` marks a ratio more than 5% from 1;
  - exit 1 on any non-equivalence, else 0.

**Tests:**
- `test_abc_prov.c` (fine mode, real engine, in-memory):
  - a LUT whose output ABC proves equal to an old internal net gets its own region and record;
  - an uncomplemented anchor moves the old net's name onto the new net (phase C), and a complemented one moves none;
  - `namemap_max_objs` = 4 → the info line with `the AIG has more than 4 objects`;
  - `--prov cone` → the info line with `--prov cone`;
  - a fake `name_map` returning `RESULT` → the line with `ABC: <text>`, and the pass still succeeds.
- Aligned build: for a `scorr` that merges registers 1 and 2 into result register 0 (*k\** = 1), the aligned MiniLUT has 2 register outputs, and register input 2 is constant 0.
- `test_abc_diff.py` (fakes): the line format, the `explain` threshold (1.051 is flagged, 1.049 is not), the exit codes.
- The flip: after it, `abc --lut 6` logs `exchange aig`; `--via-blif D` logs `exchange blif`. The flip is a separate commit, made only after `abc_diff_golden` and H1/H4 have recorded their ratios in PHASE3 (I8).
- **Deferred to the controller:** `abc_equiv_golden_aig` and `abc_diff_golden` (the k6 goldens except `mcml` and `LU32PEEng`), and the `mcml`/`LU32PEEng` legs in slots H1 and H4 (Task 22).

## PR 3D-8 — evidence

### Task 22: Determinism, `bench_abc`, calibration, the H1–H6 schedule and the PHASE3 entry (AB-26, AB-29, AB-30 §13.8–§13.9)
**Files:**
- Create `tests/bench/bench_abc.c` (Release; point (v)), `tools/abc-check/abc-determinism` (shell) and `tests/tools/test_abc_determinism.py`.
- Modify `CMakeLists.txt`:
  - gate CTests `abc_determinism_ci` and `bench_abc_smoke`;
  - the sweeps `abc_determinism_golden` and `bench_abc_set` (point (m));
  - `bench_abc` as a top-level target like `bench_util` (output `build/release/bench_abc`), linked to `odin3_core` and `odin3_abc`; its `main` calls `odin3_abc_adapter_init()` first (finding 20).
- Modify `src/passes/abc/abc.h` (the calibrated `ODIN3_ABC_BYTES_PER_AIG_OBJ`, only upwards) and `docs/PHASE3.md` (the 3D entry and checklist tick). No `nightly.yml` edit: 3H's nightly is the runner (point (m)).

**Interfaces:**
- `abc-determinism --odin3 O [--odin3-debug D] [--via-blif] [--mode a|b|c|d] <files>`:
  - (a) runs the same `read_blif; abc --lut 6 [--via-blif D]; write_blif` in two processes and compares `write_blif` and (BLIF) `abc<r>.in.blif`, `.cmd` and `.out.blif` byte for byte, never `.log` or `--log`;
  - (b) runs A, then B, then a fresh A in one `odin3 -p` script, for the default script and for `strash; fraig; if -K 6`, and compares the two A outputs;
  - (c) runs (a)'s first process, sleeps 60 s, runs it again, and compares the two (§13.8 (c));
  - (d) runs (a)'s command once with `--odin3` (Release, `build/release/odin3`) and once with `--odin3-debug` (Debug, `build/debug/odin3`) and compares `write_blif` (§13.8 (d); both build trees must exist, else exit 2). It is for the micros only (the committed BLIFs and the k6 `regression/` goldens): a path under a `vtr/` golden directory is refused with exit 2 (re-review N1);
  - it prints `same` or `differs <file>` per design. Exit 0 when all are the same, else 1. It passes the caller's environment through unchanged and, on golden paths, calls `qor_heavy.require_heavy()` first.
- `bench_abc --one <file.blif> [--lut K] [--via-blif DIR] [--exe ABC]` prints the columns of point (v). `bench_abc_set` runs it on the k6 goldens except `mcml` and `LU32PEEng`, smallest first.

**Tests and runs:**
- `abc_determinism_ci` (gate; Review Focus 5): (a) and (b) on `tests/golden/blif/*.blif` and `tests/unit/data/abc/*.blif`, less `tests/abc/equiv_expect_fail.txt`'s entries → every design `same`.
- `bench_abc_smoke`: `--one` on `tests/golden/blif/ff.parmys.blif` prints every column of point (v), the times as numbers.
- `test_abc_determinism.py` (fakes): the compared files exactly, `.log` never compared, the exit codes; mode (c) sleeps through an injected `sleep`; mode (d) without `--odin3-debug` → exit 2.
- **Nightly** (ruling I10): (a) on the golden set, (c) and (d) run in 3H's nightly "3D sweeps" step, with `abc-equiv` and `abc-diff` (point (m)).
- **Controller sweeps**, smallest first, under `tools/heavy-run`:
  - `abc_determinism_golden` (a);
  - `bench_abc_set`. Its per-run table goes verbatim into PHASE3, with VTR's ABC-stage time per design beside it.
  - The §12 targets, each reported `ok` or `MISSED`: outside-ABC time ≤ 5% of ABC's (≤ 10 s on `mcml`); linked against the executable ≤ 1.05× time with equal LUT, FF and level counts; the name map ≤ 15%; `--via-blif` overhead ≤ 25% on `mcml`; peak RSS ≤ before + 1.2 × the executable's peak + 64 MB; `open` + `close` ≤ 20 ms; the checker ≤ 1 ms on 64 KiB.
  - A missed target is investigated and reported with its phase times, never relaxed.
- **Calibration:** `ODIN3_ABC_BYTES_PER_AIG_OBJ` becomes ⌈1.5 × the worst measured (peak RSS growth ÷ AIG objects)⌉ over the 19, only if that is above 512, in its own commit citing the bench line.
- **The large designs (§13.9)**, by hand, one per heavy slot, nothing else heavy on the machine. Each command is `ODIN3_HEAVY_OK=1 tools/heavy-run timeout 3h <cmd>`. H1 and H4 are two slots each, `abc-equiv --step abc` and then `--step equiv --timeout 7200` on the same `--keep` (ruling I9), so neither exceeds 3 h:

  | Run | After | Command (`<d>` ∈ {`mcml`, `LU32PEEng`}, smallest first) |
  |---|---|---|
  | H1 | 3D-5 | `tools/abc-check/abc-equiv --step abc --via-blif ~/odin3-ws/work/abc-h1/<d>/x --keep ~/odin3-ws/work/abc-h1/<d> …`, then `--step equiv --timeout 7200` with the same `--keep` (the BLIF side of `abc-diff`, determinism (a) run 1) |
  | H2 | 3D-5 | `build/release/bench_abc --one <G> --lut 6 --via-blif ~/odin3-ws/work/abc-h2/<d> --exe build/release/third_party/abc/abc` |
  | H3 | 3D-5 | H1's `--step abc` again into `abc-h3/<d>`, then `cmp` of `after.blif`, `.in.blif`, `.cmd` and `.out.blif` with H1's |
  | H4 | 3D-7, before the flip | `abc-equiv --step abc --exchange aig --keep ~/odin3-ws/work/abc-h4/<d> …`, then `--step equiv --timeout 7200` (the in-memory side of `abc-diff`) |
  | H5 | 3D-7 | `bench_abc --one <G> --lut 6` (in-memory; the executable row reused from H2) |
  | H6 | 3D-7 | H4's `--step abc` again, then `cmp` with H4's `after.blif` |

  `--xl` (`odin3-heavyxl.slice`, 9G; `ODIN3_HEAVY_XL_OK=1`; idle machine only) is used only after `bench_abc` has measured more than 6 GB and Peter has seen the figure. A failed slot is repeated only after a fix. Each `~/odin3-ws/work/abc-h*/<d>` directory is removed with one absolute `rm -r` path after its result is recorded.
- **The PHASE3 entry** records:
  - the bench table and the H1–H6 summaries verbatim;
  - the `abc-diff` ratios on the 19 and their explanation (I8);
  - the spike's findings link;
  - M1's status (3H-3's P′ run on the BLIF exchange);
  - points (a)–(z) as agent defaults;
  - Peter's ruling on point (a)'s question and what was built from it;
  - the ABI v4 additions, with no bump.
