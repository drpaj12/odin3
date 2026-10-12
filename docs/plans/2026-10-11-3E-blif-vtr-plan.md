# 3E `write_blif --vtr` — Implementation Plan (v2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

v1 is written together with the 3D and 3H plans (`docs/plans/2026-10-11-3D-linked-abc-plan.md`, `docs/plans/2026-10-11-3H-qor-harness-plan.md`). The three share one contract for PR order, heavy runs, `$lut`, the ISOP signature, the run information and the shared files. Where this plan names a 3D or 3H interface, that plan's task owns it.

v2 applies the pre-flight review `3-plans-review` (3E: 0 critical, 0 important, 2 minor; both applied, none rejected) and the controller's shared rulings: I3 (tools pass the caller's environment through unchanged and never set `ODIN3_HEAVY_OK`; point (a)) and I4 (every VPR run goes through `tools/heavy-run`, so Task 11's runs wait for 2E-1). Finding 24 fixes `vtr-read-check`'s default binary (`build/release/odin3`); finding 25 states who rebases `writer.c` between 3E-1 and 3E-5 (conflict note 6).

**Goal:** `write_blif --vtr` writes the design's top module as BLIF that VPR reads with `-start vpr` (O and P′ legs) and that VTR's ABC stage reads with `-start abc` (O′ and R legs). It refuses, with located errors and before any byte is written, every construct VPR would reject or misread. Also in 3E:
- a VPR-free checker that gives CI the same verdicts;
- the arch's `<models>` committed as JSON, with a library check;
- the plain writer's constant convention (`gnd`/`vcc`/`unconn`);
- `$lut` written through an irredundant cover (`odin3_blif_isop`), right after 3D's `$lut` lands;
- round-trip and VPR-read evidence over every `status=ok` k6_frac golden;
- provenance names for the Oprov leg.

Not in 3E: decomposing a `.names` wider than K, `flatten`, `hardmap`, clock inversion, `.eblif`, a C arch reader.

**Architecture:** The 1C BLIF writer gains a dialect switch. `odin3_blif_write_with(design, path, opts)` keeps the plain dialect byte for byte except for 3E-3's constants and 3E-5's `$lut`. For the VTR dialect it first runs `odin3_blif_vtr_preflight` (`src/backends/blif/vtr.c`), and opens the path only when the preflight passes. The preflight:
- walks the top once, nodes in ID order, then nets;
- gives each input pin and output-port bit a source class (§9.1);
- logs every refusal E1–E13 (at most 20, then a count line);
- fills `odin3_blif_vtr_report`.

The emitter (`writer.c`) reuses 1C's name machinery with VB-10's legalisation hook. It writes, in order:
- a run-independent header;
- the ports;
- the generators `gnd`/`vcc`/`unconn` that are referenced;
- the cells in node ID order (Yosys gate covers, normalised `$sop`/`$lut` covers, `.latch … re`, `.subckt` with formals sorted as Parmys sorts them);
- the port buffers;
- the hard-block stubs, sorted by name.

`isop.c` is a Minato–Morreale ISOP on an explicit stack. The tools are stdlib Python:
- `tools/blif-vtr-check` has its own lexer mirroring libblifparse;
- `tools/vtr-models` reads the arch XML and calls VTR's own `determine_*` readers;
- `tools/vtr-read-check` runs VPR `--exit_before_pack on` on every golden under `tools/heavy-run`;
- `blif-roundtrip --vtr` and `netlist-compare --vtr-normalize` give the round-trip oracles.

**Tech Stack:** C17 and `src/util`; `src/ir` (netlist view, `odin3_net_const_value`, `odin3_celltype_lib`, `odin3_prov_sources`); the 1C writer and reader (`src/backends/blif`, `src/frontends/blif`); the 1G tech library (`lib/vtr.o3lib`, `odin3_techlib_cell`); 3D's `$lut` cell (3D-1); 3D's pass-run information (3D-3, `odin3_passrun_info`); Unity; Python 3 stdlib (`ruff`, `mypy --strict`, `unittest`); VTR `3c9a4d23` read-only (VPR, libblifparse, `run_vtr_flow.py`, the venv); `tools/heavy-run` and `tools/build-locked` (2E-1); 3H's `qor_heavy` (3H-1).

**Spec:** `docs/specs/2026-10-11-3E-blif-vtr-design.md` **v2.1**. It is binding: VB-n, R-n, E-n, C-n and § numbers below refer to it, and its §11 and §15.1 texts are verbatim requirements.

Also:
- `docs/specs/2026-10-11-3D-linked-abc-design.md` v2: AB-2 (`$lut`), AB-3 (the ISOP split), §8.1 and §8.5 (run information), §14.1's requirements answered by 3D.
- `docs/specs/2026-10-11-3H-qor-harness-design.md` v2.1: §7.1 (`qor_heavy`), §3.5 (the R leg), §13 step 5 (3E's sweeps in the nightly).
- `docs/PHASE3.md`: the "3E rules" paragraph, decisions 2, 5, 6, 9, 12, 14, 18, 19 and 20, and risks 3, 8 and 9.
- The 1C spec and `src/backends/blif/writer.[ch]`; the 1G spec and `lib/vtr.o3lib`; `docs/IR.md` IR-3, IR-4, IR-7b, IR-12 and §5.3 (names in output); the 2D spec §14.6; the 1F Verilog writer's `--name-style`.
- The standing rules, including the heavy-run rule.
- The oracle is VTR `3c9a4d23`: `libs/EXTERNAL/libblifparse/src/blif_lexer.l` and `blif_parser.y`, `vpr/src/base/read_blif.cpp`, `read_circuit.cpp`, `libs/libarchfpga/src/arch_util.cpp`, and the k6_frac and EArch XMLs. Parmys's conventions are read from the goldens.

**Ordering and bases.** `main` (c1e3987) has 1C, 1F, 1G, 2D-1 (#54), the 3D spec (#53), the 2E/2F plans (#52) and the 3E/3H specs (#51). In flight:
- 2C-2 (`feat/2C-read`): `read_verilog`, `--parse-only`, `CMakeLists.txt`, `builtin.c`, `PASSES.md`, `src/cli/main.c`;
- 2D-2 (`feat/2D-const`): `src/frontends/elab/`, `CMakeLists.txt`, `pyproject.toml`;
- 2E-1 (`feat/2E-core`, in review): `tools/heavy-run`, `tools/build-locked`, `cmake/Odin3Sweep.cmake`, `src/passes/proc/`. **Its PR merges to main first, and every heavy task of Phase 3 depends on it**: here, 3E-4's sweeps and their CTests.

Six PRs, as spec §18:

| PR | Branch | Base | Tasks | Waits for |
|---|---|---|---|---|
| 3E-1 writer | `feat/3E-writer` | `origin/main` now | 1–5 | **Nothing; starts now** (1C, 1G merged). |
| 3E-2 checker | `feat/3E-checker` | `origin/main` now | 6–7 | **Nothing; starts now** (parallel with 3E-1). |
| 3E-3 plain constants | `feat/3E-plain` | `main` after 3E-1 | 8 | 3E-1: it shares the generator code. |
| 3E-5 `$lut` | `feat/3E-lut` | `main` after 3D-1 | 9 | **3D-1 only** (the `$lut` cell). It lands right after 3D-1 and before 3D-5, so P′/M1 never waits on it. |
| 3E-4 evidence | `feat/3E-evidence` | `main` after 3E-1 and 3E-2 | 10–11 | 3E-1, 3E-2, 3H-1 (`tools/vtr-qor/qor_heavy.py`), and 2E-1 on main (`tools/heavy-run`, `tools/build-locked`, `odin3_add_sweep_test`). Task 10 starts after 3E-1; Task 11's runs wait for 3H-1 and 2E-1. |
| 3E-6 provenance names | `feat/3E-provnames` | `main` after 3E-1 | 12 | 3E-1. Any time before M3; only the Oprov leg needs it. |

3D needs 3E only through 3E-5 (`odin3_blif_isop`, consumed by 3D-3, 3D-5 and 3D-7). Its P′ leg writes `--vtr`, so M1 also needs 3E-1. 3H consumes 3E-1 (legs O, O′, P′, R), 3E-2 (`vtr-models check-lib`, `blif-vtr-check`), 3E-4 (nightly sweeps) and 3E-6 (Oprov).

**Model/effort** (PHASE0 §9; PHASE3 row 3E: light review, Opus `high`; spec §18):

| Task | Implementer | Reviewer |
|---|---|---|
| 1 Options, `odin3_blif_write_with`, pass arguments | Opus `high` | Opus `high` |
| 2 Names (VB-10) | Opus `high` | Opus `high` |
| 3 Preflight: source classes, E1–E13, report, cap | Opus `high` | Opus `xhigh` (every refusal pairs with a VPR verdict) |
| 4 Emission: header, generators, gates, `$sop`, latches, buffers | Opus `high` | Opus `high` |
| 5 Hard blocks, stubs, fixtures | Opus `high` | Opus `high` |
| 6 `blif-vtr-check` | Opus `high` | Opus `high` |
| 7 `vtr-models`, models JSON, `blif-bad` corpus | Opus `high` (negative corpus: Sonnet `high`) | Opus `high` |
| 8 Plain constants | Opus `high` | Opus `high` |
| 9 ISOP and `$lut` | Opus `high` | Opus `xhigh` (irredundance, determinism) |
| 10 `blif-roundtrip --vtr`, `--vtr-normalize` | Opus `high` | Opus `high` |
| 11 `vtr-read-check`, the k6 sweeps, report | Opus `high` (scripts); Sonnet `high` (operating the sweeps in the background with Monitor, the report) | Opus `high` |
| 12 Provenance names | Opus `high` | Opus `high` |
| Whole-branch review before **each** PR (3E-1 … 3E-6) | — | **Fable**, else Opus `xhigh` |

Effort cues:
- `/model opus` + `/effort high` at the start of each 3E-1/2/3/5/6 task session.
- `/model sonnet` + `/effort high` for Task 7's negative corpus and for Task 11's sweep sessions.
- `/clear` between tasks (one task per session).

**Lanes** (tasks are serial where they share a file other than `CMakeLists.txt` appends):
- **3E-1:** 1 → 2 → 3 → 4 → 5. All edit `writer.c` or `vtr.c` and `test_blif_vtr.c`.
- **3E-2:** 6 → 7, in parallel with all of 3E-1. Task 7's corpus is checked by Task 6's checker.
- **3E-3:** 8 after 3E-1 merges.
- **3E-5:** 9 after 3D-1 merges. It may land before or after 3E-1; point (b) says who adds the `--vtr` `$lut` row.
- **3E-4:** 10 after 3E-1; 11 after 10, 3E-2, 3H-1 and 2E-1.
- **3E-6:** 12 after 3E-1, any time before M3.

**Pre-flight conflict note.**
1. **`CMakeLists.txt`.** Append-only hunks, keep both sides:
   - Task 1 adds `src/backends/blif/vtr.c` to `odin3_core` and `odin3_add_unit_test(test_blif_vtr)` (with `tests/unit/blif_vtr_test.c` through `target_sources`);
   - Task 5 adds CTest `blif_vtr_fixtures`; Task 7 adds `vtr_models_check` and `blif_vtr_check_bad`;
   - Task 9 adds `isop.c` and `test_blif_isop`;
   - Task 10 adds `blif_roundtrip_vtr_fixtures`; Task 11 adds the `sweep` CTests through `odin3_add_sweep_test` only.
   - In flight: 2C-2 (~54 lines), 2D-2 (15), 2E-1 (14: `ODIN3_HEAVY_RUN` and the `Odin3Sweep.cmake` include). 3D-2 rewrites the ABC block, so `$<TARGET_FILE:abc>` becomes `$<TARGET_FILE:odin3_abc_exe>`; 3E references neither.
2. **`include/odin3/odin3.h`: no 3E change.** The writer is reached through the pass (spec §17). 3D-2/3D-3/3D-6 add the engine ABI, the pass-run accessors and `odin3_pass_set_abc_script`; 2D Tasks 10/17 extend their own line. 3E reads run information through the internal `odin3_passrun_info` (`src/ir/prov.h`, 3D-3), not the ABI.
3. **`src/passes/builtin.c`, `builtin.h`.**
   - Task 1 replaces `write_blif_run`'s `one_path` call with the option parser and rewrites `WRITE_BLIF`'s help. `processes` stays `ODIN3_PASS_PROCESSES_REFUSE` (2D Task 4).
   - In flight: 2C-2 adds `read_verilog`. Later, 2E Task 3 adds `proc`, 2F Task 4 adds eight opt passes and 2F Task 13 factors `compact`, and 3D-5 adds `&odin3_pass_abc` (defined in `src/passes/abc/run.c`).
   - Independent lines; keep both.
4. **`docs/PASSES.md`.** Task 1 rewrites the `write_blif` row (point (q)). 2C-2, 2D, 2E, 2F and 3D-5 (`abc`) add rows. Rows are independent lines.
5. **The `$lut` registry entry** (`src/ir/cells/lut.c`, `cells/builtin.c`, `cells/cells.h`): 3D-1's. 3E-5 consumes `odin3_cell_lut`, the `WIDTH`/`LUT` parameter names, `ODIN3_LUT_MAX_WIDTH` and the AB-2 bit order. It never assumes `$lut`'s built-in ID: 2E-1 Task 1 also appends eleven cells, and the order of the two merges decides the ID. 3E-5 edits no registry file.
6. **`src/backends/blif/writer.[ch]`.** 3E-1 (Tasks 1–5), 3E-3 (Task 8) and 3E-5 (Task 9) own it in turn. 3E-5 may land before or after 3E-1: the second of the two to land rebases the other's `writer.c` hunk, the plain `$lut` branch included, as point (b) already says for the `--vtr` row (finding 25). 3D does not edit them: its exchange writer is `src/passes/abc/xblif.c`.
   - The `# abc:` header loop (point (c)) is added by whichever of 3E-1 and 3D-3 lands second. If that is 3D-3, its implementer edits `vtr.c`'s header function and `test_blif_vtr.c` exactly as point (c) states.
   - 2D Task 4's process refusal at the writer's entry stays first in `odin3_blif_write_with`.
7. **`src/backends/blif/isop.[ch]`** (Task 9) is consumed by 3D-3 (Verilog `$lut`), 3D-5 (BLIF exchange) and 3D-7 (AIG builder). Its signature is frozen by the shared contract; a change needs all three plans.
8. **`pyproject.toml` and `tests/tools/__init__.py`.** Task 6 appends `tools/blif-vtr-check`, Task 7 `tools/vtr-models` and Task 11 `tools/vtr-read-check` to `[tool.ruff] src`, to `[tool.mypy] mypy_path` and to the `sys.path` tuple. 3H-1 appends `tools/vtr-qor` and 3D-5 `tools/abc-check`. In flight, 2D-2 and 2E-1 append `tools/elab-check` and `tools/proc-check`. Append entries; keep both.
9. **`tools/netlist-compare/netlist_compare.py`.** Task 10 adds `--vtr-normalize` (a pre-canonicalisation rewrite). 2E Task 10 adds `--implicit-models`. Separate flags and functions; keep both.
10. **`tools/blif-roundtrip/blif-roundtrip` and `odin3-blif-rt.c`.** Task 10 adds `--vtr`. Nothing else in flight touches them.
11. **`tools/vtr-qor/qor_heavy.py`** (3H-1) is used only: `reexec_under_lock` from `vtr-read-check`, and its `verify-scope` command from `blif-roundtrip` (point (a)). 3E never edits `tools/vtr-qor/`.
12. **Heavy-run files** (`tools/heavy-run`, `tools/build-locked`, `cmake/Odin3Sweep.cmake`) are 2E-1's. 3E never copies or edits them. Task 11's CTests and every controller sweep wait for them on main.
13. **The project grammar `abc_script`** (3D-6: `src/frontends/project/*`, `tests/golden/projects/*`, `project_fixtures.py`): **no 3E change.**
14. **Docs.**
    - `docs/DESIGN.md` §9 BLIF row: Task 5. 3D edits §4.0, §5.2 and §6.
    - `docs/ORACLES.md`: Task 11 adds the `vpr --exit_before_pack on` oracle. 3D-2 and 3H edit other paragraphs.
    - `docs/PHASE3.md` is append-only: Task 8 marks decision 20's sub-bullet; Task 11 adds the 3E entry. 3D and 3H append their own entries.
    - `docs/IR.md`: no 3E edit; Task 12 cites §5.3.
15. **`tests/golden/blif/`** (copied goldens) is read-only for 3E. Task 5's fixtures point at its files through `tests/golden/blif-vtr/sample.txt` (point (r)) instead of copying them.

**Spec points made exact here** (agent defaults, reversible; Task 11 logs them in PHASE3):

- (a) **Heavy runs go through `tools/heavy-run`** (the standing rule's slice correction). §16.4's `flock … systemd-run --user --scope -p MemoryMax=6G …` predates the slice correction and is not implemented. One implementation, 3H's `qor_heavy` (3H spec §7.1; 3H Task 1's Interfaces), serves both heavy 3E tools:
  - "Inside a heavy run" is `tools/heavy-run`'s own nesting evidence, checked by `qor_heavy.inside_heavy_run()`: `ODIN3_HEAVY_SLICE` is set, the process is in that slice, and the inherited descriptor 9 holds the heavy lock.
  - `vtr-read-check` (Python) calls `qor_heavy.reexec_under_lock(sys.argv[1:])` first. Inside a heavy run it checks the cap (`verify_scope`) and continues. Outside, it re-runs itself as `<repo>/tools/heavy-run <python> -I <script> …` (one exclusive lock, the 6G `odin3-heavy.slice`) with the caller's environment unchanged, when the caller set `ODIN3_HEAVY_OK=1`; otherwise it exits 2 with `vtr-qor: heavy runs are the controller's: set ODIN3_HEAVY_OK=1` (ruling I3).
  - `blif-roundtrip --vtr` on a golden root (bash) first runs `python3 -I <repo>/tools/vtr-qor/qor_heavy.py require-heavy` (3H Task 1): exit 0 → it continues; exit 3 (not inside) → it execs `<repo>/tools/heavy-run "$0" "$@"` when `ODIN3_HEAVY_OK=1` is set, else prints the same message and exits 2; exit 2 (inside, cap check failed) → it exits 2.
  - No 3E tool sets `ODIN3_HEAVY_OK`. The sweep CTests are `odin3_add_sweep_test(<name> LABEL sweep COMMAND <tool> …)`: `heavy-run` takes the lock and the tool finds itself inside.
  - Under 3H's nightly (one held heavy run), the nightly starts these tools with the lock descriptor (`pass_fds=(9,)`), so they are inside too. No 3E tool starts a nested `heavy-run` without that descriptor.
  - Children (odin3, VPR, netlist-compare) are plain subprocesses inside the scope.
  - Nothing in 3E needs more than 6 GB; `--xl` is never used.
- (b) **The `--vtr` `$lut` row** (§6) and its normalisation tests go in **whichever of 3E-1 and 3E-5 lands second**. 3E-5 depends on 3D-1 only. It also makes the **plain** writer write `$lut` as `.names` with the ISOP cover; without that, 1C's generic path would write `.subckt $lut`, and 3D §13.3 item 1's round trip would fail.
- (c) **The `# abc:` header lines** (§5 item 1): one `# abc: <info>` per pass run whose name is `abc` and whose `odin3_passrun_info` is non-zero, in run order, with `<info>` verbatim (3D §8.5's `script=… sha256=… abc=… K=… exchange=… depth=…`). They are added by **whichever of 3E-1 and 3D-3 lands second**, with test `vtr_header_abc_runs`. Until then `--vtr` writes no `# abc:` line. The script path is printed exactly as 3D recorded it (as given), never re-resolved.
- (d) **First location** (§10 item 7, 3E-6): IR.md §5.3's existing rule, used as it stands. For a `DERIVED` record it is the first leaf of the backward walk in parent order. 3D fixes its parent order (an old region's cells in node-ID order; a multi-region record's parents in anchor order), so the choice is deterministic. 3E-6 adds no rule.
- (e) **E1b by name as well as by registry.** `$_DFFSR_PPP_` is not registered (tests/golden/blif README), so a Parmys golden's async gate flip-flop is an implicit black box.
  - E1b applies to any cell type whose name matches `^\$_(DFFSR|DFFSRE|SR|ALDFF|ALDFFE)_` or `^\$_DFF_[NP][NP][01]_$`, and `^\$_DFFE_[NP][NP][01][NP]_$`.
  - It also applies to the registry word flip-flops with an asynchronous input: `$adff`, plus IR change D's `$adffe`, `$aldff`, `$aldffe`, `$dffsr` and `$dffsre`, matched by name so the rule holds before or after 2E Task 1.
  - E1b wins over E5 and E6.
  - In §16.4's pairing table, **E1b on a golden pairs with VPR's `Subckt instantiates model …`**: the async flip-flop reaches the golden as a `.subckt`, not a `.latch`. The table's "cannot occur in a BLIF golden" holds only for `.latch`.
- (f) **One refusal per node**, in this precedence: E1b, E1, E2, E3, E4, E5, E6, E10, E7, E8, E11. E3b is checked per clock pin of a hard cell that passed the type checks. E9 is checked per `$port_inout` node and E12 per net after all nodes. E13 is checked first and alone: with no top, nothing else is checked.
- (g) **E7 counts stored inputs, before normalisation.** A `$sop` or `$lut` with more than K inputs is refused even when its function is constant, which keeps §16.3's "92 files → E7" pairing exact. Normalisation (§6) therefore evaluates only covers with k ≤ K ≤ 16.
- (h) **E12's driver list** names the driving cells in node ID order (`$port_in` bits count as drivers, named by the port), at most two, then `, …` when there are more:
  - `net 'n' has 2 drivers ('$c4', '$c9')`;
  - `net 'n' has 3 drivers ('$c4', '$c9', …)`.
- (i) **`<loc>`** is `<path as given>:<line>` from the first SOURCE or IMPORTED record `odin3_prov_sources` yields: the BLIF reader's directive line, the file as the reader was given it. Otherwise it is the module name. Never `--diag-paths resolved`.
- (j) **Clock pins at `unconn` (class U)** are written as `unconn`, with `write_blif: --vtr: <loc>: <type> '<cell>': clock is 'unconn'; VPR will never clock it`. A latch C or a hard-cell clock on constant 0 or 1 gets §7 item 3's warning. For a hard cell the text is `<loc>: <type> '<cell>': clock input '<port>' is the constant <0|1>; VPR will never clock it`.
- (k) **Warning and info levels.**
  - WARN: the constant-clock lines, the `unconn` line, and the port rename (§10 item 5).
  - INFO: the legalised-names count (§10 item 9), "modules besides the top", and the summary.
  - With no `.subckt`, the summary reads `0 .subckt` with no parentheses.
- (l) **Argument parsing** (`odin3_pass_arg_next` tokens, left to right):
  - A token starting with `-` that is not an option → `unknown argument`. A repeated option → `given twice`, at its second occurrence.
  - `--lut-k` or `--name-style` as the last token → the value error with `got ''`.
  - After the scan: no path or two paths → `expects one path`, then the `--vtr` dependency check.
- (m) **`--name-style provenance` before 3E-6** parses, and `odin3_blif_write_with` refuses it with `write_blif: --vtr: --name-style provenance is not available yet (3E-6)` (`INVALID_ARG`, no file). Task 12 removes the refusal.
- (n) **The API's own refusals:**
  - `odin3_blif_write_with` with `PLAIN` and a non-zero `lut_k` or `PROVENANCE` → `odin3_blif_write_with: lut_k and name_style apply to the VTR dialect only`;
  - `lut_k` outside 2..16 → `odin3_blif_write_with: lut_k <k> is outside 2..16`;
  - both `INVALID_ARG`, no file.
- (o) **VB-10 escapes are upper-case hex.** `%` followed by two upper-case hex digits is a byte. `%` followed by anything else is the reserved-name prefix. So `%fe` (the name `fe`) and `%FE` (the byte 0xFE) differ, the map is injective, and `--vtr-normalize` decodes it exactly (Task 10).
- (p) **Plain-mode generators** (3E-3):
  - In every written module, a pin on a net driven by `$_CONST0_`/`$_CONST1_`/`$_CONSTX_`/`$_CONSTZ_` writes `gnd`/`vcc`/`unconn`/`unconn`. The `$_CONST*_` node writes no line.
  - Each referenced generator is emitted once per module, after the ports.
  - A non-constant net whose chosen name would be `gnd`, `vcc` or `unconn` falls back to `$n<ID>`.
  - A port bit on a constant net gets 1C's port buffer from the generator.
  - §9.3's zero-input `$sop` rule applies as in the VTR dialect.
- (q) **The `docs/PASSES.md` `write_blif` row** becomes: `write_blif [--vtr] [--lut-k K] [--name-style short|provenance] <file>` | 1 (1C, 1D); 3 (3E) | netlist → — | `src/backends/blif/{writer,vtr,isop}.c`, `src/passes/builtin.c` | `blif_roundtrip_fixtures`, `blif_vtr_fixtures`, `cli_*_roundtrip`, `cli_top_written_first` | Plain: the top first, then the others, written atomically; constants as `gnd`/`vcc`/`unconn` (3E-3). `--vtr`: the top alone in the dialect VPR reads (3E spec), refused with located errors before anything is written.
- (r) **Fixture inputs from the copied goldens.** `tests/golden/blif-vtr/sample.txt` lists repo-relative paths: `tests/golden/blif/adder_hard_block.parmys.blif`, `ansiportlist_2.parmys.blif`, `ff.parmys.blif`, `dffsre.parmys.blif` and `pow.parmys.blif`. For each, the expected output is `tests/golden/blif-vtr/<stem>.vtr.blif` or `<stem>.err`, where `<stem>` is the basename without `.blif`. The fixture driver runs each case from the input's directory under its basename, so `<loc>` lines are checkout-independent.
- (s) **The header's first line** is compared by pattern `^# Odin III \S+ write_blif --vtr \(K=<K>, names (short|provenance)\)$` in fixtures, so a version bump does not churn them. The rest of each file is byte-exact.
- (t) **ISOP order.** The recursion splits on input k−1 first, down to input 0, at every level, and emits the three sub-covers in this order: the `0` branch, the `1` branch, then the shared (`-`) branch. "Shorter" means fewer rows; on a tie the on-set wins.
- (u) **Sweep parallelism.** Inside the 6G slice, `blif-roundtrip --vtr` and `vtr-read-check` run one file at a time (`-j 1`), with netlist-compare's per-file cap at `-m 5`. Each file's run directory under `~/odin3-ws/work/vtr-read-check/` is removed after it passes.
- (v) **The PHASE3 checklist line** "3E `write_blif --vtr`: Parmys goldens round-trip through it and route via `-start abc`". Task 11 records the round-trip and VPR-read figures. The line is ticked when 3H's R leg routes all 19 (3H-3/3H-4), and the 3E entry says so.
- (w) **E11's `<list>`** names, in definition order, the parameters whose node value differs from what the reader would derive (the declared model's, else those the written formals imply), each in `'…'`, joined by `, `.
- (x) **§14.1's requirements are answered and tested by 3D**, not by a 3E task:
  - items 2 and 4 (`unconn`/x bypass, no logic pin on `unconn`) by `test_abc_unconn` and 3D-5's `ch_intrinsics` P′ check;
  - item 1 (the exchange's value rules) by 3D's `xblif.c` tests;
  - item 3 (`$_DFF_P_`, C re-stitched, INIT 0/1/2) by `test_abc_splice` and `test_abc_init1`;
  - item 5 (path as given plus SHA-256) by `test_passrun`;
  - item 6 (3H's depth request) by `abc: final: depth <d>` and the run information's `depth=`.

  3E's own guard is class U's warning (Task 4), which must stay silent on the O and P′ legs. 3H's nightly reports any occurrence.
- (y) **Spec §21's four decisions** (x → `gnd` on logic, used stubs only, declared-index port bits, `--lut-k` default 6) are implemented as recommended and logged by Task 11 as agent defaults for Peter.

## Global Constraints

- **Earlier constraints.**
  - C17 with the `-Werror` flag set.
  - Functions ≤ 60 lines, CCN ≤ 15, ≤ 5 parameters, no adjacent swappable parameters (use request structs).
  - **No recursion** in `src/`: the ISOP uses an explicit frame stack of depth ≤ k.
  - No VLA; no `goto` but `cleanup:`; `abort` only in IR check.
  - No `NOLINT`; no `.clang-tidy` or lizard change; never `--no-verify`.
  - `odin3_` snake_case; named constants; only `src/util` containers.
- **No public ABI change** (spec §17). New types live in `src/backends/blif/writer.h`, `vtr.h` and `isop.h`.
- **Determinism (VB-13).** The output is a pure function of the IR and the options:
  - no hash-map iteration order, time, host or working-directory path reaches a file;
  - cells in node ID order, stubs sorted by name in byte order, generated names from IDs only;
  - two runs give byte-identical files.
- **Refusal leaves no file.** A `--vtr` refusal returns `ODIN3_ERR_INVALID_ARG` before the path is opened. An existing file at the path is untouched (nothing is opened). Plain writes stay atomic (1C).
- **Diagnostics.**
  - The exact §11 texts after `write_blif: --vtr: `, or the argument texts without it, and the §15.1 checker texts.
  - Style as 2D §9: lower case, names in `'…'`, no final period.
  - At most `ODIN3_BLIF_VTR_MAX_ERRORS` (20) refusal lines, then `write_blif: --vtr: N more errors not shown`. `report.errors` is exact.
  - Tests capture the log through `odin3_log_set_sink` and assert whole lines.
- **Caps (VB-12):**

  | Cap | Value |
  |---|---|
  | `ODIN3_BLIF_VTR_MIN_K` … `ODIN3_BLIF_VTR_MAX_K` | 2 … 16 |
  | `ODIN3_BLIF_VTR_DEFAULT_K` | 6 |
  | `ODIN3_BLIF_VTR_MAX_ERRORS` | 20 |
  | line width | 100 columns, ` \` continuation; cover rows never wrap |
  | ISOP stack | ≤ k frames; truth tables ≤ 1,024 words (k = 16) |
  | preflight memory | two `u64map`s (net → class, net → legal name) and one `vec` of driver counts, freed on return |
- **Tests first.**
  - Unity under ASan/UBSan.
  - An OOM sweep (`odin3_util_set_alloc_fail_after`) over each allocating entry point a task adds: `NO_MEMORY`, no leak, no file.
  - Gates before each commit: `tools/build-locked --preset debug -j6 && ctest --preset debug -j4` green (before 2E-1 merges: `cmake --build --preset debug -j6`) and `tools/lint.sh` PASS.
  - `/lint` before each commit; commit at each green checkpoint (the controller pushes).
  - A read-only critique per task; a **Fable** whole-branch review before each PR.
- **Heavy runs** (standing rules; point (a)):
  - The k6 sweeps, any VPR run over the golden set, and `equiv-check` over the Odin II set run only through `tools/heavy-run` or a sweep CTest.
  - They run smallest first, one file at a time, by the controller.
  - Implementers run unit tests, the gate and the fixture CTests only, and list the sweeps they deferred.
  - `rm` only with one absolute path under `~/odin3-ws/work/` or `/tmp/claude-1000/`.
  - `~/odin3-ws/external` and `~/odin3-ws/golden` are read-only.
- **Python** (`tools/`, `tests/tools/`): stdlib only, `ruff check`, `mypy --strict`, `unittest`, run with `python3 -I`. VTR's own Python (`$VTR_ROOT/.venv/bin/python`) is called only by `vtr-models extract`.

## Review Focus

1. **A Parmys golden written with `--vtr` is the same netlist.** Its `unconn` wiring, `gnd`/`vcc` buffers, `.subckt` formal order and generators come back unchanged, so the R leg measures nothing but names and order. Pinned in Task 5 (`blif_vtr_fixtures`: `adder_hard_block.parmys.vtr.blif` and `ansiportlist_2.parmys.vtr.blif`, byte-exact) and Task 10 (`blif_roundtrip_vtr_fixtures`: `netlist-compare` exit 0 and W1 == W2).
2. **x on a LUT input is 0, x on a hard-block input is open — never the reverse.** A RAM address bit on `$_CONSTX_` must stay `unconn`, and a `.names` input on `$_CONSTX_` must become `gnd`. Pinned in Task 3 (`test_blif_vtr`: `classes_by_pin_kind`) and Task 5 (`ram_slice_unconn_addr` fixture).
3. **Names VPR cannot read become legal, distinct and decodable.** `a#b`, a blank, `=`, `re`, `0`, `.end`, `out:x`, `fe` and the byte 0xFE all map to distinct legal names that `--vtr-normalize` decodes back. Pinned in Task 2 (`legal_name_table`, `legal_name_injective_fe`) and Task 10 (`test_netlist_compare`: `vtr_normalize_decodes_names`).
4. **A constant on an output port has exactly one driver.** A zero-input `$sop` driving an output-port net is written as the generator under the port bit's name with no buffer, and a `$_CONST1_` on a port gets one buffer from `vcc`. Pinned in Task 4 (`const_sop_on_output_one_driver`, `const_cell_on_output_buffered`) and Task 5 (the `const_output` fixture under `blif-vtr-check --strict`, C9).
5. **A golden VPR would reject is refused with a located error, and nothing is written.** An `fe` latch, an async flip-flop arriving as `.subckt $_DFFSR_PPP_`, or a 7-input `.names` gives the E1/E1b/E7 line with its `file:line`. No file is created, and an older file at the path is unchanged. Pinned in Task 3 (`refusal_keeps_old_file`), Task 5 (`dffsre.parmys.err`) and Task 11 (the §16.4 pairing on all 923 goldens).

**Internal risks for the reviewers** (spec §19):
- **R12 block names.** Generated names for unconnected outputs keep block names unique; pinned in Task 5 (`subckt_unconnected_outputs_named`) and C8 in Task 6.
- **Scalar against indexed formals** (§8.2), which IR-7b's reader check decides; pinned in Task 5 (`ram_one_bit_slice_scalar`) and Task 10's read-back.
- **The plain-mode change** (§9.4) touches 1C/1F behaviour; pinned in Task 8 (`plain_const_generators`, the unchanged `blif_roundtrip_fixtures`).

---

## PR 3E-1 — the `--vtr` writer

### Task 1: Options, `odin3_blif_write_with`, pass arguments (VB-4)
**Files:**
- Modify `src/backends/blif/writer.h`: add §4.1's `odin3_blif_dialect`, `odin3_blif_name_style`, `odin3_blif_write_opts`, the four `ODIN3_BLIF_VTR_*` constants and `odin3_blif_write_with`, verbatim, with their paragraphs.
- Modify `src/backends/blif/writer.c`:
  - `odin3_blif_write` becomes `odin3_blif_write_with(design, path, NULL)`;
  - the writer state gains `const odin3_blif_write_opts *opts` and `bool vtr`;
  - VTR runs the preflight before the temporary file is created.
- Create `src/backends/blif/vtr.h` with §4.1's `odin3_blif_vtr_report`, `odin3_blif_vtr_preflight` and `odin3_blif_vtr_legal_name`, verbatim. Create `vtr.c`. At Task 1 the preflight checks E13 and the options only; Tasks 2–5 complete it.
- Modify `src/passes/builtin.c`: `write_blif_run` and the `WRITE_BLIF` help.
- Modify `docs/PASSES.md` (point (q)) and `CMakeLists.txt`.
- Create `tests/unit/test_blif_vtr.c`, and `tests/unit/blif_vtr_test.h`/`.c` (helpers, below).

**Interfaces:**
- Consumes: `odin3_pass_arg_next`, `odin3_design_top`, 1C's writer internals, 2D Task 4's `odin3_design_refuse_processes`.
- Produces:
  - `odin3_status odin3_blif_write_with(const odin3_design *design, const char *path, const odin3_blif_write_opts *opts);` (§4.1 contract; point (n)'s API refusals; NULL `opts` = `{PLAIN, SHORT, 0}`).
  - `odin3_status odin3_blif_vtr_preflight(const odin3_design *design, const odin3_blif_write_opts *opts, odin3_blif_vtr_report *report);` (§4.1; `lut_k` 0 resolves to 6 before the walk).
  - In `vtr.h`, for Tasks 3–5: `uint32_t odin3_blif_vtr_k(const odin3_blif_write_opts *opts);` (the resolved K).
  - Pass usage (spec §4.3), with the help string exactly: `write_blif [--vtr] [--lut-k K] [--name-style short|provenance] <file>: write the design as BLIF (the top first); --vtr writes the top alone in the dialect VPR reads`.
  - Parsing follows point (l). File-static `typedef struct write_blif_args { odin3_strbuf path; odin3_blif_write_opts opts; uint8_t seen; } write_blif_args;` and `static odin3_status write_blif_parse(odin3_bytes args, write_blif_args *out);`.
  - Test helpers (`tests/unit/blif_vtr_test.h`):
    - `typedef struct bvt { odin3_design *design; char dir[256]; odin3_strbuf log; uint32_t errors, warnings; } bvt;`
    - `odin3_status bvt_read(bvt *t, const char *blif_text, bool vtr_lib);` writes `t.blif` into a fresh directory under the build tree, `chdir`s there, runs `read_techlib <repo>/lib/vtr.o3lib` when `vtr_lib`, then `read_blif t.blif`, and restores the working directory.
    - `odin3_status bvt_write(bvt *t, const char *pass_args, odin3_strbuf *out);` runs `write_blif <pass_args> out.blif` in `t->dir` and reads the file into `out` (empty when none).
    - `bool bvt_file_exists(const bvt *t, const char *name);`
    - `void bvt_free(bvt *t);`
    - The log sink appends `<level>: <msg>\n` lines to `t->log`.

**Tests (`test_blif_vtr.c`):**
- **Arguments** (each `INVALID_ARG`, the exact line, no `out.blif`):
  - `--bogus out.blif` → `write_blif: unknown argument '--bogus'`;
  - `--vtr` → `write_blif: expects one path`;
  - `--vtr a.blif b.blif` → `write_blif: expects one path`;
  - `--vtr --lut-k 1 out.blif` → `write_blif: --lut-k expects an integer in 2..16, got '1'`; the same for `17`, `six` and `6x`;
  - `--vtr out.blif --lut-k` → `… got ''`;
  - `--vtr --name-style long out.blif` → `write_blif: --name-style expects short or provenance, got 'long'`;
  - `--lut-k 6 out.blif` → `write_blif: --lut-k and --name-style need --vtr`;
  - `--vtr --vtr out.blif` → `write_blif: option '--vtr' given twice`;
  - `--vtr --name-style provenance out.blif` → point (m)'s line (until Task 12).
- **Order free:** `out.blif --lut-k 5 --vtr` writes, and the header says `K=5`.
- **Plain unchanged:** every existing `test_blif_writer` case and `blif_roundtrip_fixtures` pass without edits, and for every `tests/golden/blif/*.blif` the bytes of `odin3_blif_write_with(d, p, NULL)` equal those of `write_blif out.blif`.
- **API (point (n)):**
  - `odin3_blif_write_with(d, p, &(odin3_blif_write_opts){PLAIN, PROVENANCE, 0})` → `INVALID_ARG` and the line;
  - `{VTR, SHORT, 1}` → `odin3_blif_write_with: lut_k 1 is outside 2..16`;
  - NULL `opts` equals `odin3_blif_write`.
- **E13:** a design with modules and no top, `--vtr out.blif` → `write_blif: --vtr: no top module (read_blif sets one; or run hierarchy)`, `INVALID_ARG`, no file.
- **Help:** `odin3_pass_at(i)->help` for `write_blif` equals the exact string.
- **Processes:** a design holding 2D's fixture P1 → the 2D refusal line first, no preflight line.

### Task 2: Names (VB-10)
**Files:** Modify `vtr.c` (legalisation, reserved names, the clock rule), `writer.c` (1C's naming hook: legalise before uniqueness, the declared-index port bits in the VTR dialect) and `test_blif_vtr.c`.

**Interfaces:**
- Consumes: 1C's `take_name`/`take_numbered`/`generate` path; the IR wire spec `{msb, lsb}`.
- Produces:
  - `odin3_status odin3_blif_vtr_legal_name(odin3_bytes name, odin3_strbuf *out);` (§4.1) applies rules 1–3:
    - bytes outside `0x21`–`0x7E`, and `=` `"` `#` `\` `%`, become `%XX` with upper-case hex;
    - a leading `.` becomes `%2E`;
    - a whole name in `re fe ah al as NIL 0 1 2 3 gnd vcc unconn`, or one starting `out:`, gets a `%` prefix.

    Rule 3's `gnd`/`vcc`/`unconn` exception (the net is that generator) is the caller's.
  - `bool odin3_blif_vtr_reserved(odin3_bytes name);` (rule 3's list, used by the writer to skip a generator's own name)
  - `odin3_status odin3_blif_vtr_clock_name(odin3_bytes legal, odin3_strbuf *out);` (rule 8: `|` → `%7C`, the `^` of each `_^_` → `%5E`), applied to nets used as a `.latch` C or a hard-cell clock input.
- Rules 4–6:
  - A legalised name colliding with another net falls back to `$n<ID>`, `$n<ID>$1`, …
  - VTR port bit k of wire `{msb, lsb}` is named `[lsb + k]` when descending and `[lsb − k]` when ascending. The plain dialect keeps 1C's `[k]`.
  - Short names follow 1C.
- Messages:
  - WARN `write_blif: --vtr: port '<p>' written as '<legal>' (VPR cannot read '<p>')`, once per port;
  - INFO `write_blif: --vtr: N names legalised (first: '<orig>' -> '<legal>')` when N > 0, where "first" is the lowest net ID renamed.
  - `report->renamed` = N.

**Tests (`test_blif_vtr.c`):**
- **`legal_name_table`** (`odin3_blif_vtr_legal_name`):

  | Input | Output |
  |---|---|
  | `a#b` | `a%23b` |
  | `a b` | `a%20b` |
  | `x=y` | `x%3Dy` |
  | `q"` | `q%22` |
  | `a\b` | `a%5Cb` |
  | `50%` | `50%25` |
  | `é` (C3 A9) | `%C3%A9` |
  | tab | `%09` |
  | `.end` | `%2Eend` |
  | `re` | `%re` |
  | `0` | `%0` |
  | `NIL` | `%NIL` |
  | `out:x` | `%out:x` |
  | `unconn` | `%unconn` |
  | `$auto$rtlil.cc:3203:MuxGate$1084` | unchanged |
  | `single_port_ram^MEM~0-7^out~0` | unchanged |
  | `memory_controller_address~26` | unchanged |

- **`legal_name_injective_fe`:** `fe` → `%fe`, and the one-byte name 0xFE → `%FE`; they differ.
- **`clock_name_rule`:** a net `a|b` used as a `.latch` C is written `a%7Cb`; `x_^_y` → `x_%5E_y`; the same net names elsewhere are unchanged when not a clock.
- **Collision:** nets named `a#b` and `a%23b` → the second (by net ID) is `$n<ID>`.
- **Declared index:** a top `input [8:1] a` (hand IR) writes `.inputs a[1] a[2] … a[8]` under `--vtr`, and `a[0] … a[7]` in plain.
- **Port rename warning:** a port `re` → `warning: write_blif: --vtr: port 're' written as '%re' (VPR cannot read 're')` once, and `.inputs %re`.
- **Info line:** three illegal net names → `info: write_blif: --vtr: 3 names legalised (first: 'a b' -> 'a%20b')`, and `report.renamed` 3.
- OOM sweep over `odin3_blif_vtr_legal_name` and a write with renames.

### Task 3: Preflight — source classes, E1–E13, report, cap (VB-9 §9.1, VB-11)
**Files:** Modify `vtr.c`, `vtr.h` and `test_blif_vtr.c`.

**Interfaces:**
- Consumes: `odin3_net_const_value`, `odin3_net_driver`, `odin3_celltype_lib` (kind `ODIN3_TECHLIB_HARD`/`_GATE`, the port `clock` flag), `odin3_design_declared_model`, `odin3_prov_sources` (point (i)), `ODIN3_CT_TRISTATE`. 1C's file-static `params_recoverable` (E11) is exported in `writer.h` as `odin3_status odin3_blif_params_check(const odin3_design *design, const odin3_module *module, odin3_node_id node, odin3_strbuf *bad, bool *ok);`, which also appends point (w)'s list to `bad`. The plain writer's refusal keeps its current text.
- Produces (`vtr.h`):
  - `typedef enum odin3_blif_src_class { ODIN3_BLIF_SRC_SIGNAL = 0, ODIN3_BLIF_SRC_0, ODIN3_BLIF_SRC_1, ODIN3_BLIF_SRC_X, ODIN3_BLIF_SRC_UNDRIVEN, ODIN3_BLIF_SRC_U } odin3_blif_src_class;`
  - `odin3_blif_src_class odin3_blif_vtr_net_class(const odin3_module *module, odin3_net_id net);` is §9.1 by the driving cell's own function only (no tracing through buffers):
    - an invalid net (unconnected pin) or a driverless net that is not a `$port_in` bit → `UNDRIVEN`;
    - `$_CONSTX_`/`$_CONSTZ_` → `X`;
    - a zero-row zero-input `$sop` on a net named `unconn` → `U`;
    - `$_CONST0_` or a constant-0 `$sop` → `0`; `$_CONST1_` or a constant-1 `$sop` → `1`;
    - otherwise `SIGNAL`.
  - `odin3_status odin3_blif_vtr_preflight(...)`: point (f)'s order, §11's texts after `write_blif: --vtr: `, the cap, and the report counts. `names`, `latches` and `subckts` count what Task 4 will write; `x_to_gnd`, `x_to_unconn` and `unconn_logic` are counted per pin as §9.2 says; `modules_skipped` is the number of modules other than the top.
  - An internal `odin3_blif_vtr_plan` keeps the class and legal-name maps for the emitter: `odin3_status odin3_blif_vtr_plan_build(const odin3_design *design, const odin3_blif_write_opts *opts, odin3_blif_vtr_plan *plan, odin3_blif_vtr_report *report);` and `void odin3_blif_vtr_plan_free(odin3_blif_vtr_plan *plan);`. The preflight is `plan_build` plus `plan_free`.

**Tests (`test_blif_vtr.c`; inputs through `bvt_read`, so `<loc>` is `t.blif:<line>`; hand IR where BLIF cannot express the case, `<loc>` = the module name). Each refusal: `INVALID_ARG`, exactly the line `error: write_blif: --vtr: …`, no `out.blif`:**
- E1: `.latch d q fe c 0` on line 5 → `t.blif:5: $_DFF_N_ '<q net name>': falling-edge flip-flop; VPR accepts only rising-edge latches (lower inverts the clock)`. The cell name is 1C's node name, or `$c<ID>`, asserted from the read design.
- E1b:
  - `.subckt $_DFFSR_PPP_ C=c D=d Q=q R=r S=s` → `t.blif:<n>: $_DFFSR_PPP_ '$c<ID>': asynchronous set/reset flip-flop; VPR has none (lower makes it synchronous, decision 19)`;
  - hand-IR `$adff` → the same text with `top:` and `$adff` (point (e)).
- E2: `.latch d q ah c 0` → `… $_DLATCH_P_ '…': level-sensitive latch; VPR accepts only rising-edge latches`.
- E3:
  - `.latch d q 0` (untyped → `$_FF_`) → `… $_FF_ '…': flip-flop without a clock; VPR requires a clock on every latch`;
  - hand-IR `$_DFF_P_` with C unconnected, then with C on `$_CONSTX_` → the same text with `$_DFF_P_`.
- E3b: a `single_port_ram` whose `clk` pin is on `$_CONSTX_` → `… single_port_ram '…': clock input 'clk' is unconnected or x`.
- E4: `.subckt $tribuf A=a EN=e Y=y` → `… $tribuf '…': tri-state cell; VPR has no tri-state`.
- E5: `.subckt $add A[0]=a B[0]=b Y[0]=y` → `… $add '…': not a netlist cell (run lower)`.
- E6:
  - `.subckt foo a=x` (implicit) → `… foo '…': 'foo' is not a hard cell of a loaded tech library (read_techlib lib/vtr.o3lib)`;
  - a gate-kind cell of `lib/generic_gates.o3lib` → the same with its type;
  - a declared `.blackbox` model `bb` → the same with `bb`.
- E7: a 7-input `.names` → `… $sop '…': 7 inputs exceed K=6 (run abc, or pass --lut-k)`. With `--lut-k 7`, no refusal. A 7-input constant cover is still refused (point (g)). `$_MUX_` with `--lut-k 2` → `… $_MUX_ '…': 3 inputs exceed K=2 (run abc, or pass --lut-k)`.
- E8: rows `1 1` and `0 0` in one cover → `… $sop '…': cover mixes on-set and off-set rows`.
- E9 (hand IR): `inout p` → `top: port 'p': inout port; VPR has no bidirectional pins`.
- E10: two models, the top instantiating `sub` → `t.blif:<n>: instance '$c<ID>' of module 'sub': hierarchy not flattened (run flatten); VPR reads one model`.
- E11: a declared `multiply` model (`a[0..35]`, `b[0..35]`, `out[0..71]`) and a hand-IR node of it with `A_WIDTH` 8, `B_WIDTH` 36 → `top: multiply '$c<ID>': parameters 'A_WIDTH' cannot be recovered from BLIF formals` (point (w)).
- E12: two `.names` driving `n` → `top: net 'n' has 2 drivers ('<a>', '<b>')`; three drivers → `… ('<a>', '<b>', …)` (point (h)).
- **Cap:** 21 `fe` latches → exactly 20 E1 lines, then `error: write_blif: --vtr: 1 more errors not shown`; `report.errors` 21.
- **`classes_by_pin_kind`** (Review Focus 2):
  - `odin3_blif_vtr_net_class` gives `X` for a `$_CONSTX_` net, `UNDRIVEN` for a driverless wire bit, `U` for Parmys's `unconn`, `0`/`1` for `gnd`/`vcc` and constant `$sop`s, and `SIGNAL` for a buffer from `vcc` (constants are not traced through buffers).
  - The report's `x_to_gnd` counts a `.names` input on `$_CONSTX_`, and `x_to_unconn` counts a RAM `addr[3]` on `$_CONSTX_`.
- **`refusal_keeps_old_file`** (Review Focus 5): `out.blif` holds `old\n`; an E1 design → `INVALID_ARG`, and `out.blif` still holds `old\n`.
- **Clean report:** the adder fixture of Task 5 → `ODIN3_OK`, `errors` 0, and the counts equal the written file's `.names`/`.latch`/`.subckt` lines.
- OOM sweep over `odin3_blif_vtr_preflight`: `NO_MEMORY`, no leak.

### Task 4: Emission — header, generators, cells, latches, port buffers (VB-5, VB-6, VB-7, VB-9 §9.2–9.3)
**Files:** Modify `writer.c` (the VTR branch of each emitter), `vtr.c` (the header text and the summary line) and `test_blif_vtr.c`.

**Interfaces:**
- Consumes: Task 3's plan (classes, names), `odin3_version_string()`, `odin3_passrun_end`, `odin3_passrun_name` and (point (c), once 3D-3 lands) `odin3_passrun_info`.
- Produces the §5 layout:
  1. `# Odin III <odin3_version_string()> write_blif --vtr (K=<K>, names <short|provenance>)`, then the `# abc:` lines (point (c)), then a blank line;
  2. `.model <top>`;
  3. `.inputs`/`.outputs` as 1C, without `.clock`;
  4. the referenced generators in the order `gnd`, `vcc`, `unconn`, each skipped when a written zero-input `$sop` already is it (§9.3);
  5. the cells in node ID order;
  6. the port buffers;
  7. `.end`;
  8. the stubs (Task 5).

  The other-modules INFO line is `write_blif: --vtr: N modules besides the top '<top>' not written`.
- Cells (§6 table, exact rows):

  | Cell | Rows |
  |---|---|
  | `$_BUF_` | `1 1` |
  | `$_NOT_` | `0 1` |
  | `$_AND_` | `11 1` |
  | `$_OR_` | `1- 1` then `-1 1` |
  | `$_XOR_` | `10 1` then `01 1` |
  | `$_NAND_` | `0- 1` then `-0 1` |
  | `$_NOR_` | `00 1` |
  | `$_XNOR_` | `11 1` then `00 1` |
  | `$_MUX_` (pins A B S) | `1-0 1` then `-11 1` |

  `$sop` keeps its rows after normalisation.
- Normalisation (§6), for a `$sop`, gate or `$lut` with ≥ 1 input. The internal helper `bool odin3_blif_vtr_eval(const odin3_module *module, odin3_node_id node, uint64_t *tt);` returns the truth table, false for k > 16.
  - A constant function → a zero-input generator on the output net: `.names <y>` with no rows (0), or with row `1` (1). The input pins are dropped.
  - A 1-input non-constant function → exactly `1 1` or `0 1`.
- Latches: `.latch <D> <Q> re <C> <init>`, with `INIT` 0→`0`, 1→`1`, 2→`2`, 3→`2`.
  - D in class X or UNDRIVEN → `gnd` (`x_to_gnd`).
  - An unconnected Q gets a fresh generated net `$p<pin ID>`.
  - A constant or U clock → the warning of §7 item 3 or point (j).
- Pins: class 0 → `gnd`, 1 → `vcc`, X/UNDRIVEN → `gnd` (logic pins), U → `unconn` (counted in `unconn_logic`, one warning per write: `write_blif: --vtr: net 'unconn' feeds N .names/.latch pins; VPR leaves them open (run abc first)`).
- Output-port bits:
  - class 0/1/X → a buffer `.names gnd|vcc|gnd <port bit>` / `1 1` (X counted in `x_to_gnd`);
  - U → a buffer from `unconn`;
  - a net driven by a constant `$sop` or a normalised constant cell → the generator itself under the port bit's name, with no buffer (§9.2).
- Summary INFO (point (k)): `write_blif: --vtr: '<top>': N .names, M .latch, S .subckt (adder A, multiply B, single_port_ram C, dual_port_ram D), K=<K>; x bits: X to gnd, Y open (unconn)`, with zero counts dropped from the parentheses.

**Tests (`test_blif_vtr.c`; whole output files as byte strings):**
- **`gate_covers`:** one module with inputs `a b s`, outputs `y0`…`y8`, one gate of each type → the file equals the expected text: header, `.model top`, `.inputs a b s`, `.outputs y0 … y8`, then each `.names` with the table's rows, `.end`.
- **`latch_init_map`:** four `$_DFF_P_` with `INIT` 0, 1, 2, 3 → `.latch d0 q0 re clk 0`, `… 1`, `… 2`, `… 2`.
- **`latch_x_d`:** D on `$_CONSTX_` → `.latch gnd q re clk 2`, `.names gnd` emitted before the cells, `x_to_gnd` 1.
- **`latch_const_clock`:** C on `$_CONST1_` → `.latch d q re vcc 2` and `warning: write_blif: --vtr: top: $_DFF_P_ '<cell>': clock is the constant 1; VPR will never clock it`.
- **`one_input_cover_normalised`** (Review Focus 5's sibling, R5b): 1-input `$sop`s with 0, 1, 2 and 3 rows (`1 1` twice; `0 1`, `1 1`, `0 1`) → respectively `.names y` (constant 0, no input), `.names a y`/`1 1`, `.names a y`/`1 1`, and `.names y`/`1` (constant 1).
- **`constant_cover_with_inputs`:** a 2-input `$sop` `-- 1` → `.names y` / `1`; a 2-input `$sop` with no rows → `.names y`.
- **`const_sop_on_output_one_driver`** (Review Focus 4): a zero-input `$sop` (row `1`) on the net of output `y` → exactly `.names y` / `1`, no `vcc` buffer, and exactly one line of the file ends a `.names` with ` y` or is `.names y`.
- **`const_cell_on_output_buffered`:** `$_CONST1_` on output `y` and on a `.names` input → `.names vcc` / `1` once, `.names vcc y` / `1 1`, and the reader uses `vcc`.
- **`x_output`:** an undriven output `y` → `.names gnd` and `.names gnd y` / `1 1`, `x_to_gnd` 1.
- **`unconn_logic_warns`:** Parmys's `.names unconn` feeding a `.names` input and a RAM `addr[2]` → both written as `unconn`, `unconn_logic` 1, and the warning once with `N` = 1.
- **`generator_is_sop`:** Parmys's `.names gnd` (a zero-input `$sop` named `gnd`) is written as stored and no second `.names gnd` appears. A signal net named `gnd` → `%gnd`, and the generator `gnd` is separate.
- **`header_exact`:** first line `# Odin III ` + `odin3_version_string()` + ` write_blif --vtr (K=6, names short)`, then an empty second line, then `.model top`.
- **`summary_line`:** a top with two `$_AND_`, one `$_DFF_P_`, three `adder` and one `single_port_ram` (`ADDR_WIDTH` 1, `DATA_WIDTH` 1, every pin connected, `addr[0]` on `$_CONSTX_`) → `info: write_blif: --vtr: 'top': 2 .names, 1 .latch, 4 .subckt (adder 3, single_port_ram 1), K=6; x bits: 0 to gnd, 1 open (unconn)`; the gate design → `info: write_blif: --vtr: 'top': 9 .names, 0 .latch, 0 .subckt, K=6; x bits: 0 to gnd, 0 open (unconn)`.
- **`other_modules_info`:** a top plus one unused module → `info: write_blif: --vtr: 1 modules besides the top 'top' not written`, and `.model` appears once.
- **`deterministic`:** two writes give identical bytes.
- OOM sweep over a full `--vtr` write of the gate design: `NO_MEMORY`, no file, no leak.

### Task 5: Hard blocks — formals, clock pins, stubs; fixtures (VB-8, §16.2)
**Files:**
- Modify `writer.c` (the VTR `.subckt` and stubs) and `test_blif_vtr.c`.
- Create `tests/golden/blif-vtr/` with:
  - `README.md` listing each fixture and why;
  - `sample.txt` (point (r));
  - `<name>.in.blif`, `<name>.vtr.blif` or `<name>.err` for: `two_clock`, `ram_slice_unconn_addr`, `ram_one_bit_slice_scalar`, `multiply_narrow`, `odin2_init3_cont`, `const_output`, `x_output`, `subckt_unconnected_outputs_named`, and the negatives `neg_e1_fe`, `neg_e2_ah`, `neg_e3_untyped`, `neg_e5_word`, `neg_e6_implicit`, `neg_e7_wide`, `neg_e8_mixed`, `neg_e10_hier`, `neg_e12_multidriver`;
  - the expected outputs of the sample files.
- Create `tools/blif-vtr-check/vtr-fixtures` (bash shim) and `vtr_fixtures.py`.
- Modify `CMakeLists.txt` (`blif_vtr_fixtures`) and `docs/DESIGN.md` §9's BLIF row (spec §20).

**Interfaces:**
- Consumes: Tasks 1–4; `odin3_celltype_lib` (library port order, the `clock` flag, width expressions); `odin3_design_declared_model`/`_decl`; 1C's `write_blackboxes` per declaration.
- Produces:
  - **Formals (§8.2)**, for each bit k of each port at the node's width:
    - the formals are sorted by port name in byte order, then by bit index numerically;
    - a formal is scalar (`p`) when the port's width expression is the literal `1`, or the node's width for it is 1, or the declared model's scalar flag says so; otherwise it is `p[k]`;
    - an input on class X/UNDRIVEN or unconnected → `unconn` (`x_to_unconn`);
    - a clock-flagged input follows the latch C rule (E3b, or the warnings);
    - an output on no net → a fresh `$p<pin ID>` net;
    - bits beyond the node's width are not written.
  - **Stubs (§8.3)**, one per hard type used by a live node of the top, sorted by type name:
    - a declared type is written as declared;
    - an undeclared library cell is written in its library port order, each port at the maximum width over live nodes, scalar under the formal rule.
    - Each stub is a blank line, `.model <m>`, `.inputs …`, `.outputs …`, `.blackbox`, `.end`.
  - `vtr-fixtures --odin3 PATH --dir DIR [--checker PATH] [--write]`:
    - For each case it runs, from the input's directory under its basename, `odin3 -p "read_techlib <repo>/lib/vtr.o3lib; read_blif <in>; write_blif --vtr <tmp>/<name>.vtr.blif"`.
    - It compares the file with `<name>.vtr.blif` (point (s)), or the `odin3: error:` lines of stderr with `<name>.err`.
    - It then runs `blif-vtr-check --strict` on every expected `.vtr.blif`.
    - It exits 0 when all pass, 1 on any difference (a unified diff is printed), 2 for usage.
    - `--write` writes missing expected files only, never overwriting one.

**Tests:**
- **`test_blif_vtr.c`:**
  - `formals_sorted`: a `single_port_ram` read from a file whose formals are in library order is written `addr[0] … addr[14] clk data[0] … out[0] … we`.
  - `ram_one_bit_slice_scalar`: a 1-bit data slice → `data=` and `out=`.
  - `subckt_unconnected_outputs_named`: an `adder` with `cout` unconnected → `cout=$p<pin ID>`. Two such adders get distinct names.
  - `ram_clock_constant`: `clk` on `$_CONST0_` → written `clk=gnd` with `warning: write_blif: --vtr: top: single_port_ram '<cell>': clock input 'clk' is the constant 0; VPR will never clock it`.
  - `stub_width_max`: two `multiply` nodes with `a` widths 8 and 12 → a stub with `a[0] … a[11]`, in library order (`a b out`).
  - `stub_declared_order_kept`: a declared `multiply` model with ports `b a out` → that order in the stub.
  - `stub_unused_dropped`: a declared `adder` model with no live node → no `adder` stub.
- **CTest `blif_vtr_fixtures`** (`vtr-fixtures --odin3 $<TARGET_FILE:odin3> --dir tests/golden/blif-vtr --checker tools/blif-vtr-check/blif-vtr-check`; label `unit`; the `--checker` argument is added by whichever of 3E-1 and 3E-2 lands second, and without it the driver skips the checker step with a printed note):
  - `adder_hard_block.parmys.vtr.blif` (Review Focus 1) keeps the golden's `.names gnd`/`vcc`/`unconn` generators, the dummy chain head `.subckt adder a=vcc b=gnd cin=gnd cout=… sumout=…`, and every formal of the golden in the golden's order. It is checked by hand against the golden and frozen in the commit that adds it.
  - `ansiportlist_2.parmys.vtr.blif`: the `adder` and `multiply` stubs as declared, sorted `adder` then `multiply`.
  - `ff.parmys.vtr.blif`: `.latch … re clk 2`.
  - `dffsre.parmys.err` = `odin3: error: write_blif: --vtr: dffsre.parmys.blif:<n>: $_DFFSR_PPP_ '<cell>': asynchronous set/reset flip-flop; VPR has none (lower makes it synchronous, decision 19)` (point (e)).
  - `pow.parmys.err`: the E5 line for `$pow`.
  - `two_clock`: two `.latch` lines with different C, unchanged.
  - `ram_slice_unconn_addr`: `addr[5]=unconn` … `addr[14]=unconn`.
  - `multiply_narrow`: `a[0]…a[7]` only.
  - `odin2_init3_cont`: init `3` → `2`, and continuations joined in the reader then rewrapped at 100 columns.
  - `const_output` and `x_output` as in Task 4.
  - Each `neg_*` case: its one `.err` line.
- OOM sweep over a write with stubs.

## PR 3E-2 — the VPR-free checker and the arch's models

### Task 6: `blif-vtr-check` (VB-15 §15.1)
**Files:** Create `tools/blif-vtr-check/blif-vtr-check` (bash shim: `exec python3 -I "$(dirname "$0")/blif_vtr_check.py" "$@"`), `tools/blif-vtr-check/blif_vtr_check.py` and `tests/tools/test_blif_vtr_check.py`. Modify `pyproject.toml` and `tests/tools/__init__.py` (conflict note 8).

**Interfaces:**
- CLI exactly §15.1: `blif-vtr-check [--models FILE] [--lut-k K] [--strict] [--eblif] [--max-errors N] FILE...`.
  - `--models` defaults to `<repo>/tests/vtr/arch/k6_frac_N10_frac_chain_mem32K_40nm.models.json`, with `<repo>` from `Path(__file__).resolve().parents[2]`.
  - `--lut-k` defaults to the JSON's `lut_size`; `--max-errors` defaults to 50 per file.
  - Exit 0 when every file is legal, 1 on any violation, 2 for usage, I/O or an unreadable models file.
  - Violations go to stdout, one per line: `blif-vtr-check: <file>:<line>: <message>`.
- Python:
  - `@dataclass(frozen=True) class Token: kind: str; text: str; line: int` (kinds: `DIRECTIVE`, `NAME`, `STRING`, `EOL`, `LATCH_KW`);
  - `def lex(text: str) -> Iterator[Token]` mirrors `blif_lexer.l`:
    - `ID_SET` is `[^ \t\r\n\\="]`;
    - a `#` comment only where a token starts (longest match, so `a#b` is one name);
    - `\`+newline is a continuation, in every state;
    - the `LATCH` state turns `re fe ah al as NIL 0 1 2 3` into `LATCH_KW`;
    - an illegal byte raises `LexError(line, message)`.
  - `@dataclass(frozen=True) class Models: lut_size: int; models: dict[str, ModelPorts]` and `@dataclass(frozen=True) class ModelPorts: inputs: dict[str, int]; outputs: dict[str, int]; clocks: tuple[str, ...]`;
  - `def load_models(path: Path) -> Models`;
  - `@dataclass(frozen=True) class Violation: path: str; line: int; message: str`;
  - `@dataclass(frozen=True) class CheckOptions: lut_k: int; strict: bool; eblif: bool; max_errors: int`;
  - `def check_text(text: str, path: str, models: Models, opts: CheckOptions) -> list[Violation]` (C1–C9, C5b, and C-S1–C-S5 with `strict`);
  - `def main(argv: list[str] | None = None) -> int`.
- Messages are §15.1's, verbatim.
- C9 counts `.inputs` pads as drivers.
- C-S4's header is a first line matching point (s)'s pattern; "unused" means no `.subckt` of that model in the top.

**Tests (`test_blif_vtr_check.py`, inline texts; each case asserts the exact message, line and exit code through `main` on a temp file):**
- C1: `.names a=b y` → `illegal character <0x3D> in name`; `"abc` unterminated → `unterminated quoted string`; `.names a#b y` / `1 1` → no violation.
- C2: `.clock clk` → `directive '.clock' is not read by VPR`; `.attr x 1` → `'.attr' needs --circuit_format eblif`, and none with `--eblif`.
- C3: two models with logic → `second model 'm2' with primitives; VPR reads one`; `.model foo`/`.blackbox` → `black-box model 'foo' is not an arch model`; a stub `adder` with port `a[1]` → `stub port 'a' of 'adder': <reason>`, with `<reason>` = `width 2 exceeds 1`.
- C4: 7 inputs with K 6 → `.names with 7 inputs exceeds K=6`.
- C5: row `1 1 1` on 1 input → `cover row has 2 columns, expected 1` (the second row token); `2 1` → `cover row character '2'`.
- C5b: `.names a y` with rows `1 1` and `1 1` → `1-input .names 'y' has 2 cover rows; VPR asserts exactly one (absorb_buffer_luts)`; zero rows → `has 0 cover rows`.
- C6:
  - `.latch d q fe c 0` → `latch type 'fe' (VPR accepts only re)`;
  - `.latch d q re NIL 0`, `.latch d q`, `.latch d q 2` → `latch without a clock`;
  - `.latch d q re c 4` → `latch init '4'`;
  - `.latch re q re c 0` → `'re' on a .latch line lexes as a keyword`.
- C7:
  - `.subckt ram x=1` → `.subckt model 'ram' is not an arch model`;
  - `.subckt adder q=x` → `no port 'q' on model 'adder'`;
  - `.subckt single_port_ram addr[15]=x …` → `port 'addr[15]' exceeds width 15 of 'single_port_ram'`;
  - `a=x a=y` → `formal 'a' given twice`.
- C8: a `.names y` and a `.subckt adder … cout=y` (first output formal) → `duplicate block name 'y' (names and subckt)`.
- C9: `.inputs a` plus `.names b a` → `net 'a' has 2 drivers`.
- Strict only:
  - C-S1: `latch init 3 (write 2; VTR's ABC rejects 3)`;
  - C-S2: `net 'unconn' feeds a .names/.latch pin (VPR leaves it open)`;
  - C-S3: `.names gnd` / `1` → `'gnd' is not a constant generator of the right value`;
  - C-S4: `missing header`, `.clock present`, `stub 'adder' unused`;
  - C-S5: `clock net 'a|b' contains '|' or '_^_' (breaks VTR's blackbox_latches.pl)`.
- `--max-errors 2` on a file with 5 violations → 2 lines; exit 1.
- A missing file → exit 2 with `blif-vtr-check: <file>: cannot read: …`.
- Every `tests/golden/blif/*.parmys.blif` except `dffsre` and `pow` → exit 0 without `--strict`.

### Task 7: `vtr-models`, the committed models JSON, the negative corpus (VB-16 §15.2)
**Files:**
- Create `tools/vtr-models/vtr-models` (shim) and `vtr_models.py`.
- Create `tests/vtr/arch/k6_frac_N10_frac_chain_mem32K_40nm.models.json` and `tests/vtr/arch/EArch.models.json`, both made by `extract` locally and committed.
- Create `tests/vtr/blif-bad/` with one file per VPR-facing rule: `c1_illegal_char.blif`, `c2_clock.blif`, `c3_two_models.blif`, `c3_bad_blackbox.blif`, `c4_wide_names.blif`, `c5_row_width.blif`, `c5b_buffer_rows.blif`, `c6_fe.blif`, `c6_no_clock.blif`, `c6_init4.blif`, `c6_keyword.blif`, `c7_no_model.blif`, `c7_no_port.blif`, `c7_wide_port.blif`, `c7_twice.blif`, `c8_dup_block.blif`, `c9_two_drivers.blif`. Each starts with `# vpr: <the VPR message it draws>`.
- Create `tests/vtr/blif-bad/strict/` with one file per C-S rule (VPR accepts them).
- Create `tests/tools/test_vtr_models.py`. Modify `pyproject.toml`, `tests/tools/__init__.py` and `CMakeLists.txt`.

**Interfaces:**
- CLI exactly §15.2:
  - `vtr-models extract ARCH.xml [--out FILE] [--vtr-root DIR]`;
  - `vtr-models check FILE [--arch ARCH.xml]`;
  - `vtr-models check-lib LIB.o3lib [--models FILE] [--vtr-root DIR]`.

  Exit 0/1/2; messages `vtr-models: <subcommand>: <cell or key>: <reason>`. `--vtr-root` defaults to `$VTR_ROOT`, else `~/odin3-ws/external/vtr-verilog-to-routing`.
- Python:
  - `def extract(arch: Path, vtr_root: Path) -> dict[str, object]` builds §15.2's JSON:
    - `schema` 1, `arch` basename, `arch_sha256`, `vtr_commit` (`git -C <vtr_root> rev-parse HEAD`), `lut_size`, `ram_addr_width`, `models`;
    - widths follow R11 (the largest `num_pins` over the pb_types whose `blif_model` is `.subckt <m>`);
    - `lut_size` and `ram_addr_width` come from `$VTR_ROOT/.venv/bin/python -I -c '<snippet>' <arch>`, where the snippet puts `vtr_flow/scripts/python_libs` on `sys.path` and prints `vtr.log_parse.determine_lut_size(arch)` and `determine_memory_addr_width(arch)` (`-I` ignores `PYTHONPATH`, so the path goes in through `sys.path`);
    - a disagreement with its own pb_type count → `vtr-models: lut_size: VTR says <a>, the pb_types give <b>`, exit 1.
  - `def check(json_path: Path, arch: Path | None) -> list[str]` checks the schema, and with `arch` also the sha256 and the content.
  - `def check_lib(lib: Path, models: dict[str, object]) -> list[str]` parses only the `cell`/`in`/`out`/`param`/`end` lines of an `.o3lib`. For each `hard` cell it requires a model of that name, the same port names and directions, `clock` equal to the model's `is_clock`, and every default width ≤ the model's size.
  - JSON is written with `json.dumps(obj, indent=1, sort_keys=True) + "\n"`.

**Tests:**
- `test_vtr_models.py`, over a tiny committed arch XML fixture (`tests/tools/fixtures/vtr_models/mini_arch.xml`) and a fake VTR Python that prints `6\n15\n`:
  - `extract` gives `adder` `{a:1,b:1,cin:1}` / `{cout:1,sumout:1}`, the maximum `num_pins` per port across two pb_types, and `clocks` `["clk"]` for the RAM;
  - a fake printing `5` → exit 1 with the `lut_size` line;
  - `check` without `--arch` on the committed k6 JSON → exit 0, and a JSON missing `models` → exit 1 `vtr-models: check: models: missing`;
  - `check-lib lib/vtr.o3lib` against the committed k6 JSON → exit 0;
  - a library cell `adder` with an extra port `x` → `vtr-models: check-lib: adder: port 'x' is not on the model`;
  - `clk` without `clock` → `vtr-models: check-lib: single_port_ram: port 'clk' clock flag differs from the model`.
- The committed k6 JSON equals spec §2's table: `adder`, `multiply` (`a` 36, `b` 36, `out` 72), `single_port_ram` (`we` 1, `addr` 15, `data` 64, `clk` 1, `out` 64, clocks `["clk"]`) and `dual_port_ram` (32-bit data, 15-bit addresses); `lut_size` 6; `ram_addr_width` 15. The EArch JSON has the same model set.
- CTest `vtr_models_check` (label `unit`): `vtr-models check tests/vtr/arch/*.models.json` and `check-lib lib/vtr.o3lib` → exit 0.
- CTest `blif_vtr_check_bad` (label `unit`): every `tests/vtr/blif-bad/*.blif` → exit 1 with exactly one violation, whose message class matches the file's prefix. Every `strict/*.blif` → exit 0 without `--strict` and exit 1 with it. That VPR rejects exactly the top-level files is verified by Task 11's `--bad-corpus` run.

## PR 3E-3 — the plain writer's constants

### Task 8: Plain constants `gnd`/`vcc`/`unconn` (VB-9 §9.4; PHASE3 #20)
**Files:**
- Modify `src/backends/blif/writer.c` (the plain branch: point (p)), `writer.h` (the contract paragraph: constants are written as `gnd`/`vcc`/`unconn`, never `.subckt $_CONST*_`) and `tests/unit/test_blif_writer.c`.
- Update any 1C/1F fixture that expects `.subckt $_CONST…`. A grep of `tests/` and `tools/` on c1e3987 finds none; the task re-greps after rebasing and updates what appears, in the same commit.
- Modify `docs/PHASE3.md`: decision 20's x → `unconn` sub-bullet marked done by 3E-3.

**Interfaces:**
- Consumes: Task 4's generator emitter (shared with the VTR branch) and Task 3's `odin3_blif_vtr_net_class` (0/1/X; UNDRIVEN keeps 1C's `$p<pin ID>` in plain mode).
- Produces: point (p). This is the Parmys `write_blif -true + vcc -false + gnd -undef + unconn` convention for every module.

**Tests (`test_blif_writer.c`):**
- **`plain_const_generators`:** module `m` with `$_CONST0_`, `$_CONST1_` and `$_CONSTX_` feeding three `$_AND_` inputs, and `$_CONSTZ_` on output `z`. The text contains, after the ports and before the cells:
  - `.names gnd`
  - `.names vcc` / `1`
  - `.names unconn`

  The gates read `gnd`, `vcc` and `unconn`; `z` gets `.names unconn z` / `1 1`; there is no `.subckt $_CONST`.
- **Per module:** a second module using only `$_CONST1_` emits only `.names vcc` / `1`.
- **Name fallback:** a signal net named `vcc` in a module that also uses `$_CONST1_` → the signal is `$n<ID>`.
- **Round trip:** write, `read_blif`, write → identical text. The constant cells come back as zero-input `$sop`s.
- **Unchanged:** `blif_roundtrip_fixtures` and every `tests/golden/blif` round trip stay green (goldens contain no `$_CONST*_` cell).

## PR 3E-5 — `$lut` through an irredundant cover

### Task 9: `odin3_blif_isop`, plain and `--vtr` `$lut` (VB-4 §4.1–4.2, VB-6; 3D AB-2/AB-3)
**Files:**
- Create `src/backends/blif/isop.h`, `isop.c` and `tests/unit/test_blif_isop.c`.
- Modify `writer.c`: the plain `$lut` → `.names`, and the `--vtr` `$lut` row if 3E-1 has merged (point (b)).
- Modify `test_blif_writer.c` (and `test_blif_vtr.c` per point (b)) and `CMakeLists.txt`.

**Interfaces:**
- Consumes: 3D-1's `odin3_cell_lut` (`WIDTH` index 0, `LUT` index 1, BITS with LUT[i] = bit i from the LSB, pin j of `A` = input j) and `ODIN3_LUT_MAX_WIDTH`.
- Produces (`isop.h`, frozen by the shared contract):
  - `odin3_status odin3_blif_isop(const uint64_t *tt, uint32_t k, odin3_strbuf *cover, uint32_t *rows, int *constant);`
    - `tt` has `max(1, 2^k / 64)` words; bit i is f(i), and bit j of i is input j. Bits at or above 2^k in word 0 (k < 6) are ignored.
    - The result is the shorter of the on-set and off-set ISOP (point (t)), with rows appended as k plane characters then the output character and a newline.
    - A constant gives `*rows` 0 and `*constant` 0 or 1; otherwise `*constant` is −1.
    - `INVALID_ARG` for NULL or k > 16; `NO_MEMORY`.
  - `odin3_status odin3_blif_isop_set(const uint64_t *tt, uint32_t k, bool offset, odin3_strbuf *cover, uint32_t *rows);` gives one set's cover (the off-set covers ¬f with output `0`), for tests and for 3D's cover choice.
  - `uint32_t odin3_blif_isop_words(uint32_t k);` (= `max(1, 2^k / 64)`).
  - Algorithm (point (t)): Minato–Morreale `isop(L, U, n)` with `L ⊆ U` over the first n inputs.
    - n = 0 → a cube of `-`s when L ≠ 0, else nothing.
    - Otherwise split on input n−1: `C0 = isop(L0 & ¬U1, U0)`, `C1 = isop(L1 & ¬U0, U1)`, `L* = (L0 & ¬C0) | (L1 & ¬C1)`, `C* = isop(L*, U0 & U1)`.
    - The result is C0's cubes with input n−1 = `0`, then C1's with `1`, then C*'s with `-`.
    - It runs on an explicit frame stack (depth ≤ k) with per-frame tables from one arena.
- Plain writer: `$lut` → `.names A[0] … A[WIDTH−1] Y` with the cover. A constant table gives a zero-input `.names Y`, with no rows (0) or row `1` (1).
- `--vtr` row (§6): the same cover; k > K → E7 (point (g)); a constant LUT → a zero-input generator; a 1-input LUT → one row.

**Tests:**
- **`test_blif_isop`, exact covers:**
  - AND2 `tt = 0x8` → `11 1\n` (1 row);
  - OR2 `0xE` → the on-set has 2 rows (`-1 1`, `1- 1`) and the off-set 1, so the result is `00 0\n`;
  - XOR2 `0x6` → `10 1\n01 1\n` (tie, on-set);
  - identity k = 1 `0x2` → `1 1\n`; inverter `0x1` → `0 1\n`;
  - `0x0` k = 2 → rows 0, constant 0; `0xF` k = 2 → constant 1;
  - k = 0 with `tt` bit 0 = 1 → constant 1.
- **Exhaustive k ≤ 4** (every function of 0–4 inputs; 65,536 at k = 4), for both sets via `odin3_blif_isop_set` and for the chosen result:
  - the cover evaluates to f (on-set) or ¬f (off-set) on all 2^k inputs;
  - **irredundance**: removing any one row changes the evaluated function;
  - the chosen result has `min(rows_on, rows_off)` rows, and is the on-set on a tie.
- **Random k = 5…16** (`odin3_prng` seed 0x3E5, 200 tables per k): the cover evaluates to the function on 4,096 random inputs (all inputs for k ≤ 12); the run is deterministic (two calls give identical bytes).
- **Bits above 2^k ignored:** k = 2 with `tt = 0xF8` equals `tt = 0x8`.
- **No recursion, bounded memory:** k = 16 with a parity table runs; with `odin3_util_set_alloc_fail_after(n)` at every n → `NO_MEMORY`, no leak.
- **`test_blif_writer`, `lut_plain`:**
  - a `$lut` `WIDTH` 2, `LUT` `4'b1000` on pins `a b` → `.names a b y` / `11 1`;
  - WIDTH 3 majority → its exact ISOP rows;
  - a `$lut` with all-0 `LUT` → `.names y`;
  - write, `read_blif`, write → identical text, and the read-back `$sop` truth table equals the `LUT` parameter (3D §13.3 item 1).
- **`test_blif_vtr`, `lut_vtr`** (point (b)):
  - the same cells under `--vtr` → the same rows;
  - WIDTH 7 with `--lut-k 6` → E7 `… $lut '…': 7 inputs exceed K=6 (run abc, or pass --lut-k)`;
  - a 1-input identity LUT → `1 1`;
  - a constant-1 WIDTH-3 LUT → `.names y` / `1` (the §6 constant classes per width: WIDTH 1, 3 and 6).

## PR 3E-4 — round-trip and VPR-read evidence

### Task 10: `blif-roundtrip --vtr` and `netlist-compare --vtr-normalize` (VB-17 §16.3)
**Files:**
- Modify `tools/blif-roundtrip/blif-roundtrip`: `--vtr` mode, and the shim behaviour of point (a) when given a golden root rather than `--fixtures`.
- Modify `tools/blif-roundtrip/odin3-blif-rt.c` (`--vtr [--lut-k K]` → `odin3_blif_write_with`).
- Modify `tools/netlist-compare/netlist_compare.py` (`--vtr-normalize`).
- Modify `tests/tools/test_netlist_compare.py` and create `tests/tools/test_blif_roundtrip_vtr.py`. Modify `CMakeLists.txt`.

**Interfaces:**
- `odin3-blif-rt [--techlib LIB]… [--vtr [--lut-k K]] IN OUT`: exit 0 written, 1 refused (the refusal lines on stderr), 2 for an I/O or usage error.
- `blif-roundtrip --vtr [-j 1] [-t S] [-m GB] [--techlib LIB]… [--rt PATH] [--nc PATH] [--equiv PATH] [--out DIR] [--report FILE] [--fixtures DIR | GOLDEN_ROOT]`:
  - Per file, smallest first: `W1 = rt --vtr G`, `W2 = rt --vtr W1`.
  - Oracle 1: W1 == W2 byte for byte.
  - Oracle 2: Parmys files (`*.parmys.blif`) → `netlist-compare G W1` exit 0; a file the §6 normalisation changes is compared with `--vtr-normalize` and listed `normalized`.
  - Oracle 3: Odin II files (`*.odin.blif`) → `netlist-compare --vtr-normalize G W1` exit 0.
  - Oracle 4: for Odin II files written in oracle 3 with no `single_port_ram`/`dual_port_ram`/`multiply` and one clock, `equiv-check --timeout 600 G W1` (`timeout` listed, `multiclock` skipped).
  - A refused file is expected: Parmys `fe` files must give E1, and Odin II files E1/E7/E12 as §16.3 says.
  - The report is TSV: `golden\ttool\twritten|refused\tclass\tidempotent\tcompare\tequiv\tverdict`.
  - Exit 0 when every verdict passes, 1 otherwise, 2 for usage.
- `netlist_compare.py --vtr-normalize` applies, to both files before canonicalising:
  - latch init `3` → `2`;
  - an omitted `.subckt` input formal bit ≡ `=unconn` (bits up to the stub's or the model's width);
  - `.clock` lines dropped;
  - VB-10 names decoded (point (o): `%XX` with upper-case hex → the byte; `%` + anything else → drop the `%`);
  - §6's constant and 1-input normalisation (constant covers to zero-input generators, 1-input covers to one row).

  Python: `def vtr_normalize(model: BlifModel) -> BlifModel` over `tools/blif`'s model type.

**Tests:**
- `test_netlist_compare.py`:
  - **`vtr_normalize_decodes_names`** (Review Focus 3): `%fe` ↔ `fe`, `%FE` ↔ byte 0xFE, `a%23b` ↔ `a#b`, `%2Eend` ↔ `.end`;
  - a latch init 3 against 2 → equal only with the flag;
  - a `.subckt multiply a[0]=x` against one with `a[1..35]=unconn` → equal with the flag;
  - a 1-input cover with two `1 1` rows against one row → equal with the flag.
- `test_blif_roundtrip_vtr.py`, over a fake `odin3-blif-rt`:
  - smallest first;
  - a W1 ≠ W2 file fails;
  - an `fe` Parmys file not refused fails;
  - a refused one passes as `refused E1`;
  - on a golden root: `require-heavy` exit 3 with `ODIN3_HEAVY_OK=1` → the script execs `heavy-run "$0" …` (fakes for both, logging their argv, and the environment passed unchanged); exit 3 without it → exit 2 and the message; exit 0 → it runs.
- CTest **`blif_roundtrip_vtr_fixtures`** (label `unit`): `blif-roundtrip --vtr --fixtures tests/golden/blif --techlib lib/vtr.o3lib` → exit 0. Every Parmys sample is idempotent and `netlist-compare`-identical; `dffsre` and `pow` are `refused` (E1b, E5). This is §16.3 step 5 (CI).

### Task 11: `vtr-read-check`, the k6 sweeps, the report and the PHASE3 entry (VB-17 §16.4–16.6)
**Files:**
- Create `tools/vtr-read-check/vtr-read-check` (bash: `exec python3 -I "$(dirname "$0")/vtr_read_check.py" "$@"`), `vtr_read_check.py` (its `main` starts with point (a)'s `reexec_under_lock`) and `tests/tools/test_vtr_read_check.py`.
- Modify `CMakeLists.txt`: sweep CTests `blif_roundtrip_vtr_k6` and `vtr_read_check_k6`, each through `odin3_add_sweep_test(<name> LABEL sweep COMMAND <tool> …)`, registered only when `VTR_ROOT` and the golden tree exist.
- Modify `pyproject.toml`, `tests/tools/__init__.py`, `docs/ORACLES.md` (`vpr --exit_before_pack on` is the VPR-read oracle) and `docs/PHASE3.md` (the 3E entry, point (v)).

**Interfaces:**
- `vtr-read-check [--golden-root DIR] [--arch ARCH.xml] [--odin3 PATH] [--checker PATH] [--work DIR] [--report FILE] [--limit N] [--bad-corpus DIR]`:
  - The defaults: `~/odin3-ws/golden/k6_frac_N10_frac_chain_mem32K_40nm`, the k6 XML under `$VTR_ROOT`, `build/release/odin3`, Task 6's checker, `~/odin3-ws/work/vtr-read-check`, `<work>/report.tsv`.
  - It collects every `*.blif` whose sibling `.prov` says `status=ok` (923 on 2026-10-11), sorted by size, smallest first.
  - For each, in a fresh `<work>/<n>/`:
    1. `odin3 -p "read_techlib <repo>/lib/vtr.o3lib; read_blif G; write_blif --vtr W"`;
    2. `$VTR_ROOT/vpr/vpr <arch> W --circuit_format blif --exit_before_pack on` (when written);
    3. the same on G;
    4. `blif-vtr-check --strict W` and `blif-vtr-check G`.
  - The verdict is §16.4's:
    - a written W needs VPR exit 0 and the checker exit 0;
    - a refusal pairs with VPR's G verdict per the pairing table and point (e) (E3b and E8 listed as intended; E11 listed per file);
    - the checker's verdict on G equals VPR's.
  - `--bad-corpus DIR` instead runs VPR and the checker on each `tests/vtr/blif-bad` file and requires that VPR rejects exactly the files the checker rejects, with the message in the file's `# vpr:` line.
  - The report TSV header is exactly `golden\ttool\twritten|refused\terror class\tvpr_W\tvpr_G\tchecker_W\tchecker_G\tverdict`. A passing file's directory is removed with one absolute path.
  - Exit 0 when every verdict passes, 1 otherwise, 2 for usage or `verify_scope` failure.
- Python: `def pairing_ok(refusal: str, vpr_g_message: str) -> bool` (the table plus point (e)); `def run_one(g: Path, ctx: ReadCtx) -> Row`; `def main(argv: list[str] | None = None) -> int`.

**Tests:**
- `test_vtr_read_check.py`, with fake `odin3`, `vpr` and checker scripts:
  - smallest-first order;
  - `pairing_ok("E1", "Only rising edge latches supported")` is true;
  - `pairing_ok("E1b", "Subckt instantiates model '$_DFFSR_PPP_', but no such model exists in the architecture file.")` is true (point (e));
  - `pairing_ok("E4", …)` is false for every message;
  - a written W that VPR rejects → verdict `fail`;
  - checker/VPR disagreement on G → `fail`;
  - the report header exact;
  - `--bad-corpus` with a file VPR accepts but the checker rejects → exit 1;
  - outside a heavy run with `ODIN3_HEAVY_OK=1`, `main` re-runs itself through `heavy-run` (a fake logging its argv `[<repo>/tools/heavy-run, <python>, -I, <vtr_read_check.py>, …]` and an environment equal to the caller's); without the variable → exit 2 and the message;
  - inside (fake slice, cgroup and descriptor) with a fake cgroup that fails `verify_scope` → exit 2.
- **Controller sweeps** (deferred by the implementer; run after `tools/build-locked --preset release -j6`, smallest first, one file at a time):
  - `ODIN3_HEAVY_OK=1 ctest --preset sweep -R 'blif_roundtrip_vtr_k6|vtr_read_check_k6'` (before 2D Task 25's preset: `ODIN3_HEAVY_OK=1 ctest --preset release -L '^sweep$' -R '…' -j1`);
  - estimate ≈ 1 h (§16.3) + ≈ 1 h (§16.4); the first run records the real times;
  - expected outcomes:
    - every Parmys k6 golden written is idempotent and `netlist-compare`-identical (the survey predicts 463; the 4 `fe` files → E1, and async `$_DFFSR_*` files → E1b paired with `Subckt instantiates model`);
    - every Odin II golden written is identical under `--vtr-normalize` (6 `fe` → E1, 92 wide → E7, 2 multi-driver → E12);
    - VPR reads every W;
    - the checker agrees with VPR on all 923 G and every W;
    - `--bad-corpus` passes.
- **Report and PHASE3** (Sonnet `high` session, in the background with Monitor):
  - the 3E entry gives the counts written/refused per class and per tool, the run times, the files compared with `--vtr-normalize`, and the `equiv-check` results (timeouts listed);
  - it also logs this plan's points (a)–(y) as agent defaults, and the R leg's tick as 3H's (point (v)).

## PR 3E-6 — provenance names

### Task 12: `--name-style provenance` (VB-10 item 7; DESIGN §13 #6)
**Files:** Modify `vtr.c` and `writer.c` (the provenance branch; point (m)'s refusal removed), `test_blif_vtr.c` and `tests/golden/blif-vtr/` (one fixture `prov_names` built by a script on a `read_verilog` design once 2D lands, else on hand IR).

**Interfaces:**
- Consumes: IR.md §5.3's provenance-name rule and the 1F Verilog writer's builder of `hier/<short name>@file:line` (made shared as `odin3_prov_name(const odin3_design *design, odin3_prov_id id, odin3_bytes short_name, odin3_strbuf *out)` in `src/backends/prov_name.[ch]`, if 1F's is file-static; the 1F writer then calls it); `odin3_prov_sources`.
- Produces, for `--name-style provenance`:
  - Every net and cell name except top-level port bits becomes `hier/<short name>@file:line` from the object's provenance record. The location is IR.md §5.3's first location (point (d)): for `DERIVED`, the first leaf of the backward walk in parent order. The file is as given.
  - VB-10 rules 1–4 then apply: blanks in paths become `%20`, `#` becomes `%23`.
  - Objects without a location keep their short name.
  - Port bits keep their short names.

**Tests (`test_blif_vtr.c`):**
- A `.names` read from `t.blif` line 7, with net `n`, in top `top` → `top/n@t.blif:7`.
- An output port `y` keeps `y`.
- A path with a blank (`my dir/t.blif`) → `top/n@my%20dir/t.blif:7`.
- A hand-built `DERIVED` net whose parents are two SOURCE records (lines 9, then 3, in parent order) → `…@t.blif:9`.
- An object without a location → its short name.
- Two runs give identical bytes.
- `--diag-paths resolved` set on the design does not change the names (the file stays as given).
- The header says `names provenance`.
- Point (m)'s refusal text no longer appears.
- `blif_vtr_fixtures` includes `prov_names`.
