# 3H QoR harness and exit run — Implementation Plan (v2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

v1 is written with the 3D and 3E plans (same commit), which share one contract: the heavy-run convention, the run-information header, the `$lut` cell and the PR order. For a 3D or 3E interface, that plan's Interfaces block is the authority.

v2 applies the pre-flight review `3-plans-review` (3H: 1 critical, 3 important, 6 minor; every finding applied, no minor rejected) and the controller's rulings, each cited where it lands:
- C1: one job at a time (`MAX_JOBS = 1`), fcheck right after its own VPR job and never beside another; the first P run is ≈ 4–4.5 h (point (w)).
- I2: the controller hand-starts the nightly as `systemd-run --user … tools/heavy-run … vtr-qor nightly`; no delayed start, no Python-side locking (point (c)).
- I3: tools pass the caller's environment through unchanged and never set `ODIN3_HEAVY_OK` (point (a)).
- I4: no pre-2E-1 fallback: T8 and the Task 3 fixtures wait for `tools/heavy-run` on `main` (point (g)).
- I10: the nightly gains a "3D sweeps" step, the runner of 3D §13.4, §13.5 and §13.8 (Task 13).

**Goal:** Build `tools/vtr-qor`, the stdlib-Python harness that runs Parmys (P) and Odin III's legs (O, O′, P′, and the evidence legs R and Oprov) through VTR `3c9a4d23` on the 19 VTR 7.0 benchmarks, collects VTR's own metrics, and writes the Phase 3 exit table and verdict under the WSL limits and the heavy-run rule.
- The P leg needs nothing from Odin III. It runs as soon as 3H-1 lands and 2E-1's `tools/heavy-run` is on `main` (≈ 4–4.5 h solo, one job at a time).
- P′ reads the verified Parmys golden, so M1 needs only 3D's `abc` and 3E's writer.
- The exit run (M3) takes ≈ 20–30 h (about 1 h more with group A serial) over 5–7 nights, resumed through the job cache.
- Nightly runs start by hand until Peter approves the systemd timer.

**Architecture:** Seven stdlib modules under `tools/vtr-qor/`:
- `qor_heavy.py`: the heavy convention (detect `tools/heavy-run`'s nesting evidence, re-run through it, verify the cgroup cap), the machine readers, `safe_rmtree`; 3D's and 3E's heavy tools use it.
- `qor_legs.py`: the one definition of every leg's commands, which 3F imports.
- `qor_sched.py`: jobs, keys, the cache, the planner, and the runner (one job at a time, group A then group B, each fcheck right after its own VPR job, with admission, timeouts, cleanup and retention).
- `qor_parse.py`: VTR's own `parse_vtr_flow.py`, plus the `time -v` and stage-log readers.
- `qor_netstats.py`: one streaming pass over the pre-VPR BLIF.
- `qor_table.py`: ratios, geomeans, the gate and the writers.
- `vtr_qor.py`: the CLI, with the bash shim `vtr-qor`.

Jobs, tables and nightly reports live in `~/odin3-ws/work/vtr-qor/`, inputs in `tests/vtr/qor/`. Only the controller publishes tables to odin3-golden.

**Tech Stack:** Python 3 stdlib (≥ 3.10; the machine has 3.12), with `ruff`, `mypy --strict` and `unittest` (run by the `python_tools` CTest).
- VTR `3c9a4d23`, read-only: `run_vtr_flow.py`, `run_vtr_task.py`, `python_libs/vtr/`, `parse/`, `mosaic/verilator_check/run_random_check.py`, and the venv `$V/.venv/bin/python`.
- Tools: GNU `time -v`, Verilator, `flock`, `systemd-run --user`, and `tools/heavy-run` (2E-1).
- From Odin III: `tools/equiv-check`, the release `odin3`, 3E's `write_blif --vtr` and `tools/vtr-models`, and 3D's `abc`.

**Spec:** `docs/specs/2026-10-11-3H-qor-harness-design.md` **v2.1**. It is binding: QH-n, § and E-QH numbers below refer to it, and its message texts (§10), CSV headers (§6.4), `bands.json` (§6.2) and `metrics.json` schema (§5.2) are verbatim requirements, except where a point below makes them exact.

Also:
- `docs/PHASE3.md`: the exit test (items 1–5) and decisions 2, 3, 5, 15, 16, 17 and 18. Decision 17 is the WSL caps.
- The 3E spec v2.1: §4.3, §5 item 1 (the `# abc:` header), §14.1 item 6, §15.2 (`vtr-models`), §16.4–16.5 (nightly step 5's sweeps, the R leg).
- The 3D spec v2: §8.1 (run information), §11 (`abc: final: depth <d>`), §6.3 (I8: the exchange P′ used), §13.9 (3D's heavy runs share the lock).
- The standing rules (heavy runs; `rm` roots; never writing `~/odin3-ws/golden` or `~/odin3-ws/external`; they live outside this repo, in the controller's SDD directory: `~/odin3-ws/odin3-2Cp/.superpowers/sdd/2026-10-11-2C-verilog-parser-plan/standing-rules.md`, and the harness enforces them in code: `check_out_dir`, `safe_rmtree`, VTR run read-only), PHASE0 #29 (WSL caps) and §9 (model/effort), and `docs/ORACLES.md` (the VTR pin).
- The 2E plan point (p) and `tools/heavy-run` / `tools/build-locked` as on `feat/2E-core`.

**Ordering and bases.** `main` (c1e3987) has the 3E/3H specs (#51), the 2E/2F plans (#52), the 3D spec (#53) and 2D-1 (#54). In flight: 2C-2 (`feat/2C-read`), 2D-2 (`feat/2D-const`) and 2E-1 (`feat/2E-core`), whose Task 2, in review, creates `tools/heavy-run`, `tools/build-locked` and `cmake/Odin3Sweep.cmake`. **That PR merges to main first; every real 3H run depends on it.**

Four PRs and one controller run, as spec §17:

| PR | Branch | Base | Tasks | Waits for |
|---|---|---|---|---|
| 3H-1 harness + P leg | `feat/3H-harness` | `origin/main` now | 1–6 | **Nothing for the code: it starts now** (unit tests use fakes). T8, the Task 3 fixture captures and every real run need 2E-1's `tools/heavy-run` on `main` (ruling I4, point (g)). |
| 3H-run-P the first P run | — (controller) | `main` after 3H-1 | 7 | 3H-1 merged, 2E-1 on `main`; solo, ≈ 4–4.5 h. |
| 3H-2 table + gate | `feat/3H-table` | stacked on `feat/3H-harness` after Task 6 | 8–9 | 3H-1. Task 9's close-out needs 3H-run-P's `metrics.json`. |
| 3H-3 control legs, checks, nightly | `feat/3H-legs` | `main` after 3H-2 | 10–13 | 3H-2, 3E-1 (`--vtr`), 3E-2 (`vtr-models`), 3E-5 (`$lut` in `--vtr`), 3D-5 (`abc`, for P′; its golden tools for the "3D sweeps" step). `crosscheck` needs 3F, else it moves to Task 14 (point (h)). Closed by the M1 run. |
| 3H-4 exit | `feat/3H-exit` | `main` after 3H-3 | 14–15 | 2D, 2E, 2F (and 2H's VTR rows), 3A, 3B, 3C, 3G; 3D (3D-7 for the measured exchange, point (e)); 3E-6 (Oprov); 3F. Closed by M2 and M3. |

**Model/effort** (PHASE0 §9; PHASE3 row 3H: light review, Opus `high`, Sonnet for bulk and runs):

| Task | Implementer | Reviewer |
|---|---|---|
| 1 `qor_heavy` (lock, cap, readers, `safe_rmtree`); 5 `qor_sched` runner; 8 the gate | Opus `high` | Opus `xhigh` (cgroups, kills, the verdict) |
| 2 legs, keys, planner; 3 `qor_parse`; 4 `qor_netstats`; 6 CLI and T8; 9 writers; 10 P′/R; 12 `equiv`; 14 O′/O/Oprov | Opus `high` | Opus `high` |
| 7 the first P run (controller) | Sonnet `high` | the controller |
| 11 `fcheck` | Opus `high` (`divergences.tsv` research: Sonnet `high`) | Opus `high` |
| 13 `nightly` …, the M1 run | Opus `high` (M1: Sonnet `high`) | Opus `high` |
| 15 `exit-run`, M2, M3, the exit table | Opus `high` (runs: Sonnet `high`) | Opus `high`; Peter reads the exit table |
| Whole-branch review before **each** PR (3H-1 … 3H-4) | — | **Fable**, else Opus `xhigh` |

Effort cues:
- Say `/model opus` + `/effort high` at the start of each code task.
- Say `/model sonnet` + `/effort high` for every run session (Task 7, M1, the group B measurements, M2, M3, hand-started nights). A run session starts the command in the background and watches `harness.log` with Monitor until `vtr-qor: done:`, never waiting hours in the foreground.
- Say `/clear` between tasks (one task per session).

**Lanes:**
- **3H-1:** 1 → 2 → {3 ‖ 4} → 5 → 6. Tasks 3 and 4 touch disjoint files; Task 5 imports both, Task 6 everything.
- **3H-run-P:** Task 7, after the merge.
- **3H-2:** 8 → 9.
- **3H-3:** 10 → 11 → 12 → 13. All four edit `vtr_qor.py` and `qor_sched.py`.
- **3H-4:** 14 → 15.
- Lane 3 runs alongside 3D/3E (lane 1) and 3A/3B/3C (lane 2).

**Pre-flight conflict note.**
1. **`tools/heavy-run`, `tools/build-locked`, `cmake/Odin3Sweep.cmake` (2E-1 Task 2, in review).** 3H executes `heavy-run` and never copies it, and uses its nesting rule (inside a heavy run, `heavy-run <cmd>` runs `<cmd>` as is when descriptor 9 holds the lock). Every real run (T8, the Task 3 fixture captures, 3H-run-P, M1, the group B measurements, nightly, M2, M3) depends on 2E-1 being on `main`; there is no fallback (ruling I4). 3H registers **no** `odin3_add_sweep_test` CTest. Builds use `tools/build-locked --preset release -j6`, never under `heavy-run`.
2. **`CMakeLists.txt` and `.github/workflows/ci.yml`: no edit.** The `python_tools` CTest runs `unittest discover -s tests/tools`, so `test_vtr_qor_*.py` join the gate by themselves. 2C-2, 2D-2, 2E-1, 3D and 3E edit `CMakeLists.txt` without touching 3H.
3. **`pyproject.toml` and `tests/tools/__init__.py`.** Task 1 appends `tools/vtr-qor` to `[tool.ruff] src`, `[tool.mypy] mypy_path` and the `sys.path` list. 2D-2 (`tools/elab-check`), 2E-1 (`tools/proc-check`), 3D-5 (`tools/abc-check`), 3E-2 (`tools/blif-vtr-check`, `tools/vtr-models`) and 3E-4 (`tools/vtr-read-check`) append too; keep both sides.
4. **`include/odin3/odin3.h`, `src/passes/builtin.c`, `docs/PASSES.md`, the cell registry, `src/frontends/project/*`: no change.** 3H adds no C and no pass; it consumes Odin III only as the release binary (`odin3 --help`'s pass list, point (t); the §3 scripts; the `--vtr` file and its header). 3D's `$lut` reaches 3H only as `.names` lines, which `qor_netstats` counts like any other. The project grammar's `abc_script` (3D-6) reaches it only as the header's `sha256=` word (the key input "the ABC script's SHA-256"); 3H reads no project file, and the O leg uses `lib/abc/vtr.abc`.
5. **The `# abc:` header** is written by whichever of 3E-1 and 3D-3 lands second, as `# abc: script=<path as given> sha256=<64 hex> abc=<40 hex> K=<K|none> exchange=<aig|blif> depth=<d>`. Task 4 parses exactly that. Before it exists, `abc_depth` and `abc_exchange` are empty for O/P′.
6. **`tools/equiv-check` is consumed, never edited** (`--timeout 600`; Task 12). 3D-5's `--blackbox` and 2H's later `--x-report` are not used: §8.2 compares O′ with O, unboxed.
7. **3E's and 3D's tools.** `tools/vtr-models` (3E-2) is run by `check-lib`, `doctor` and nightly step 1. 3E-4's `vtr-read-check` calls `qor_heavy.reexec_under_lock`, and the shell `blif-roundtrip --vtr` calls `qor_heavy.py require-heavy`; nightly step 5 runs both. 3D's `abc-equiv`, `abc-diff` and `abc-determinism` call `qor_heavy.require_heavy()` on golden paths; the nightly's "3D sweeps" step runs them (3D point (m), ruling I10). The nightly starts every such tool with `pass_fds=(9,)`, so it is inside the held run. Task 1's signatures are their interface, and a change edits those tools in the same PR.
8. **3F** imports `qor_legs.odin3_script` (frozen from 3H-1 on), and `crosscheck` runs 3F's task configs.
9. **3D's own heavy runs** (3D §13.9, H1–H6) share the one lock with 3H's runs; the controller orders them. The P′ runs are 3H's.
10. **Docs.**
    - `docs/PHASE3.md` (append-only): Task 9 (the P baseline, decision 17's note), Task 13 (M1), Task 15 (M2, M3, the exit table). 3D and 3E append too; keep both.
    - `docs/ORACLES.md`: Tasks 9 and 11. 3D-2 and 3E-4 edit other paragraphs.
    - `docs/DESIGN.md` §13 #6: Task 15.
    - The odin3-golden README section is the controller's commit.
11. **`~/odin3-ws/work/vtr-qor/`** is data outside the repo. Implementers write only test temp directories under `/tmp/claude-1000/`.

**Spec points made exact here** (agent defaults, reversible; Task 15 logs them in PHASE3):

- (a) **The heavy wrapper is `tools/heavy-run`** (standing rules; the shared contract). §7.1's `flock … systemd-run --user --scope -p MemoryMax=6G …` predates the named-slice correction: a bare scope escapes the cap of an enclosing one.
  - **Inside a heavy run** means `heavy-run`'s own nesting evidence: `ODIN3_HEAVY_SLICE` is set, the process's cgroup lies under that slice, and the inherited descriptor 9 is the heavy lock (`/proc/self/fd/9` resolves to `/tmp/claude-1000/odin3-heavy.lock`). `qor_heavy.inside_heavy_run()` checks exactly that; the review's finding 27 (both pieces of evidence) holds. The spec's `ODIN3_HEAVY=1` marker is not used.
  - `qor_heavy.reexec_under_lock`: inside, it calls `verify_scope` and returns. Outside, it requires `ODIN3_HEAVY_OK=1` in the **caller's** environment, else `vtr-qor: heavy runs are the controller's: set ODIN3_HEAVY_OK=1` (exit 2; ruling I3). Then it runs `[<repo>/tools/heavy-run, ("--xl"), sys.executable, "-I", <script>, *argv]` with the caller's environment unchanged and exits with the child's status. `<script>` is `vtr_qor.py` for the harness; 3E's tools get their own `sys.argv[0]`.
  - `heavy-run` takes `/tmp/claude-1000/odin3-heavy.lock` exclusively (writer preference over builds), caps `odin3-heavy.slice` at `MemoryMax=6G`, `MemorySwapMax=0`, and runs the tool in its scope `odin3-heavy-<pid>-<ns>.scope` inside the slice. The jobs inherit that scope and never call `systemd-run` themselves. No harness code starts a nested `heavy-run`; a heavy child tool is started with `pass_fds=(9,)`.
  - It first probes the lock with `flock -n`. When it is busy it prints `vtr-qor: waiting for the heavy lock /tmp/claude-1000/odin3-heavy.lock`, or with `--no-wait` exits 4 (E-QH4) without starting `heavy-run`.
  - `heavy-run`'s exit 77 (skipped: a 4 h lock wait, or no systemd user manager) maps to E-QH3's second text, made exact: `cannot apply the memory cap (tools/heavy-run exited 77; see its message above)`, exit 2.
- (b) **`verify_scope` walks the cgroup ancestors**, because `heavy-run` caps the slice and the scope's own `memory.max` reads `max`.
  - It reads the `0::<path>` line of `/proc/self/cgroup` and walks `/sys/fs/cgroup/<path>` up to the root.
  - It passes when some ancestor has a numeric `memory.max` ≤ the limit (6,442,450,944; 9,663,676,416 under `--xl`) and some ancestor has `memory.swap.max` = `0`.
  - `oom_kill` is read as a delta of the capping directory's `memory.events` around each job. `memory.peak` is read from the job's **own leaf cgroup** (the `heavy-run` scope), because the slice persists across heavy runs and its peak is not this run's (finding 26); one job runs at a time, so the scope's peak is the job's.
  - E-QH3 prints the tightest `memory.max` found (else `max`) and the own `memory.swap.max`.
- (c) **The hand-started nightly** (§13; Peter has not approved the timer; ruling I2). The controller starts it when he wants it to run, with no delayed start and no Python-side locking:

  ```
  systemd-run --user --unit=odin3-vtr-qor-nightly-<YYYYMMDD> --collect --setenv=ODIN3_HEAVY_OK=1 \
    -p RuntimeMaxSec=19800 <repo>/tools/heavy-run <repo>/tools/vtr-qor/vtr-qor nightly [--budget H]
  ```

  - `RuntimeMaxSec=19800` (5.5 h: the 5 h budget plus 30 min for the last tool's SIGTERM grace) stops the service, and so releases the lock, if a tool outlives the night's own deadline (3H-3 review H3); the unit file `tools/vtr-qor/systemd/odin3-vtr-qor-nightly.service` carries the same line.
  - The transient service outlives the Claude session. `heavy-run` takes the lock, creates the scope and exports `ODIN3_HEAVY_SLICE`, so `nightly` finds itself inside (point (a)) and starts at once.
  - Its deadline is the earlier of the next 06:00 local and the start plus `--budget` (default 5 h).
  - `ODIN3_HEAVY_SERVICE`, `--start-at` and the in-Python `flock` are gone. The timer files ship in `tools/vtr-qor/systemd/` for Peter, running the same `heavy-run … nightly` command; no agent installs them.
- (d) **`--xl` is controller-only and never automatic.**
  - Only `run --designs <capped designs> --xl` and `fcheck --designs … --xl` accept it (finding 29); `exit-run` and `nightly` never do. It goes through `tools/heavy-run --xl`: `odin3-heavyxl.slice`, 9G. The name has no dash, because systemd nests dashed slice names inside `odin3-heavy.slice`.
  - Without `ODIN3_HEAVY_XL_OK=1` it gives `--xl needs ODIN3_HEAVY_XL_OK=1 (an idle machine; the controller starts it)` (E-QH3, exit 2).
  - It is only for jobs already `capped`, after their measured peak went to Peter (§7.1: the agent never raises the cap), and only on an idle machine.
  - The harness never retries under `--xl` by itself. `job.json` records the slice, and the Cost section marks the row `xl`. The key is unchanged.
- (e) **`abc_depth` and the exchange for O, P′, Oprov** (3D §8.1).
  - They come from the **last** `# abc:` line of `<J>/<d>.blif`, read before cleanup: `depth=` gives `abc_depth`, and `exchange=` gives the new `netlist.abc_exchange` (`null`/`"aig"`/`"blif"`). That is the only addition to §5.2's schema; `runs.csv` keeps §6.4's header.
  - P, O′ and R keep VTR's `abc0.out` `lev = N`.
  - `qor.md` names each P′/O row's exchange (3D I8: QoR is measured on the in-memory exchange; M1 may use BLIF).
- (f) **Two keys** (§5.2 vs §5.3: a key that includes the design cannot also name a directory holding every design).
  - The **job key** is §5.3's SHA-256 over every listed input. It is stored as `metrics.json` `key` and decides cache reuse.
  - The **leg key** is the same canonical JSON without `design` and the design-source SHA-256. Its first 12 hex digits are the `<key12>` directory, `latest`, and `table --keys`. "Complete" means every design of the run's set has an `ok` job under that `key12`.
  - The common parameters enter both keys as `-track_memory_usage -crit_path_router_iterations 100` plus `--seed N` when N ≠ 1. The housekeeping parameters (`-temp_dir`, `-timeout`, `-delete_*`) are left out: they hold the job directory (circular) and do not change results.
- (g) **No pre-2E-1 fallback** (ruling I4; finding 4). T8, the Task 3 fixture captures and every other VPR run go through `tools/heavy-run`, so they wait for 2E-1 on `main`. The controller captures Task 3's fixtures; the implementer commits them.
- (h) **`crosscheck`** lands in Task 13 if 3F's `tests/vtr/tasks/` exists when 3H-3 opens. Otherwise it moves to Task 14 unchanged.
- (i) **`sets.json` carries §2's first five columns** in a `designs` map beside the spec's lists (Task 2), so nightly admission can use `vtr_flow_time_s` × 1.5 when no measured time exists.
- (j) **The pool.** Each worker thread runs one job's commands with `subprocess.Popen(…, start_new_session=True)`. Results are read back by key, so completion order never reaches a table.
- (k) **E-QH10's `stage` and `peak`.** `stage` is the newest stage log by mtime (`parmys.out`, `abc*.out`, `*_blackboxing_latch.out`, `restore_latch*.out`, `vpr*.out`, `frontend.log`, `fcheck.log`). `peak` is the job's leaf scope's `memory.peak` in MiB (point (b)), or `?`.
- (l) **Parmys determinism** (spec §20 item 3, report-only). Before cleanup, a P job's `job.json` gets `parmys_blif_sha256` (when VTR left `run/<d>.parmys.blif`) and `parmys_equals_golden` (`true`/`false`/`null`).
- (m) **`--out` roots.** `--out` must resolve under `~/odin3-ws/work/` or `/tmp/claude-1000/`, else `refusing --out <dir>: not under ~/odin3-ws/work or /tmp/claude-1000` (E-QH12, exit 2). `safe_rmtree`'s root is the resolved `--out`.
- (n) **Nightly step 3 keeps the O and O′ netlists** (`--keep-netlists`), because its `equiv` and `rerun-check` need them.
- (o) **P′'s front-end cost** is `parmys.out` of the latest P job with the same VTR commit and arch, plus `frontend.time`. When that job is missing, the cell is `*` with `vtr-qor: Pprime <d>: no P job for the Parmys stage cost`.
- (p) **`--allow-vtr-drift` is report-only.** `job.json` records `"vtr_drift": true`, and `gate` on such a table prints `gate: INCOMPLETE: VTR at <sha12> is not the pin (report-only run)`, exit 3.
- (q) **The odin3 build id** is the SHA-256 over the `odin3` binary, then each `libodin3*.so` that `ldd` resolves (`libodin3_abc.so` included), in basename order. Tests replace `ldd` through the module-level seam `qor_sched.LDD_CMD` (`mock.patch.object`), never an environment variable: no production path reads a test hook from the environment (controller ruling, 3H Task 1 fix round 1; Task 2 fix round 1).
- (r) **Rounding.** Peak RSS is `(kB + 512) // 1024` MiB. Admission messages print disk free as an integer and `MemAvailable` with one decimal.
- (s) **Pair names.** CSV and Markdown write `O'` and `P'`. Directories and `job.json` use `Oprime`/`Pprime`. The CLI accepts both.
- (t) **Leg availability.** A pass is registered when a line of `odin3 --help` matches `^  ([A-Za-z_][A-Za-z0-9_]*)(?=[ :]|$)`. Every pass's help line starts with its name, and option lines (`  --…`) never match.
- (u) **`vtr_qor.py` puts its own directory on `sys.path`**, because `python3 -I` (3.11+) does not.
- (v) **`equiv` classification** (Task 12) reads `tools/equiv-check`'s stderr:
  - exit 2 with `unsupported: more than one clock domain` → `multiclock`;
  - exit 2 with `running ABC failed:` and `timed out` → `timeout`;
  - any other exit 2 → `error`;
  - the harness's own 660 s cap expiring → `timeout`.
- (w) **One job at a time** (ruling C1; finding 1). §7.2's and PHASE3 #17's `-j3` group A pool predates the heavy-run rule, and three jobs in one scope share one `oom_kill` counter and one peak. So `MAX_JOBS` is 1: `-j` defaults to 1 and `-j 2` or more is E-QH9 `-j <n> exceeds 1 (the heavy-run rule)`. Each fcheck runs right after its own VPR job, never beside another job, in both groups. A capped job is not retried (nothing else was running), so `CAPPED_RETRIES` is 0. Group A of the first P run is ≈ 1,185 s serial × 1.5 ≈ 30 min, group B ≈ 3.8 h: ≈ 4–4.5 h solo. Task 15 logs this against PHASE3 #17 and 3H §7.2; `-j3` would need a new ruling and per-job sub-scopes.
- (x) **`lut_size` reaches VTR's Python as 3E does** (finding 31): `$PY -I -c <snippet> <arch>`, the snippet putting `<V>/vtr_flow/scripts/python_libs` on `sys.path` and calling `vtr.log_parse.determine_lut_size`, not `PYTHONPATH`. It is 3E Task 7's helper; whichever of 3E-2 and 3H-1 lands second makes `qor_legs` import `vtr_models`' copy.

## Global Constraints

- **Python.** Stdlib only. `ruff check` clean (line 100; E, F, W, I, B, UP, SIM, C90; mccabe 15) and `mypy --strict` clean. Tests use `unittest` under `tests/tools/`, with fixtures under `tests/tools/fixtures/vtr_qor/`. No C code, no pass, no libodin3 change.
- **Every subprocess** is an argv list, never a shell string; the only shell is the shim `exec python3 -I "$(dirname "$0")/vtr_qor.py" "$@"`.
- **Messages.** The prefix is `vtr-qor: `, lower case, no final period, absolute paths. The texts are §10's (E-QH1…E-QH15) plus points (a), (d), (m), (o) and (p).
- **Exit codes:**
  - 0: every job ok (`gate`: PASS);
  - 1: a job failed, or the gate failed;
  - 2: usage, configuration or environment;
  - 3: no failure but at least one `capped` or `timeout` (`gate`: INCOMPLETE);
  - 4: lock busy with `--no-wait`;
  - 5: stopped for disk space.
- **Heavy runs** go only through `tools/heavy-run`, via `qor_heavy.reexec_under_lock` or the controller's hand start (points (a), (c)). One heavy job runs machine-wide, capped at 6 GiB with no swap, and inside it one VTR or Verilator job at a time (point (w)). No tool sets `ODIN3_HEAVY_OK` (ruling I3). `--xl` is the controller's, only on an idle machine, and only after a capped measurement has gone to Peter (point (d)). Implementers run at most T8 and list the runs they deferred.
- **Caps (§11), named constants in the module that enforces them:**

  | Constant | Value |
  |---|---|
  | `CAP_BYTES` / `CAP_XL_BYTES` (`qor_heavy`) | 6 GiB / 9 GiB, no swap |
  | `MAX_JOBS` (`qor_sched`) | 1 (point (w)) |
  | `MIN_MEM_AVAILABLE_GIB` | 7 |
  | `MIN_DISK_FREE_GIB` / `DISK_FLOOR_GIB` | 20 / 10 |
  | `PAUSE_LIMIT_S`, `POLL_S` | 7,200, 60 |
  | `STAGE_TIMEOUT_S` A / B (`qor_legs`) | 7,200 / 14,400 |
  | `JOB_TIMEOUT_S` A / B | 14,400 / 21,600 |
  | `KILL_GRACE_S` | 30 |
  | `CAPPED_RETRIES` | 0 (point (w)) |
  | `KEEP_KEYS`, `KEEP_FAILED`, `KEEP_FAILED_BYTES` | 3, 5, 2 GiB per (arch, leg) |
  | `FCHECK_VECTORS`, `FCHECK_SEED`, `FCHECK_J_A`, `FCHECK_J_B` | 50,000, 1, 2, 1 |
  | `EQUIV_TIMEOUT_S` | 600 (child wall cap 660) |
  | `NIGHTLY_DEADLINE`, `NIGHTLY_BUDGET_H` | 06:00, 5 (point (c) and Task 13; 3H-3 review L1) |
  | `GEOMEAN_MAX`, `PER_DESIGN_MAX` | from `bands.json` (1.20, 1.50), never constants in code |
- **Files.** Never write `~/odin3-ws/golden` (only `publish`, by the controller, with `ODIN3_QOR_PUBLISH=1`) or `~/odin3-ws/external`. VTR's scripts run read-only: the working directory is `<J>`, and outputs go to `--temp_dir`/`--work-dir` under `<J>`. Every deletion goes through `safe_rmtree`. No symlinks are created: `latest` is a file written by atomic rename.
- **Determinism.** The job list, the keys and every table file are pure functions of `sets.json`, the job directories and their `metrics.json`/`job.json`; dates and hosts never come from the clock. JSON is written with `sort_keys=True`, `indent=1`; CSV per RFC 4180 with `\n` line ends.
- **Gates:** `tools/build-locked --preset debug -j6 && ctest --preset debug -j4` green (before 2E-1 merges, `cmake --build --preset debug -j6`) (the `python_tools` test included) and `tools/lint.sh` PASS. Run `/lint` before each commit; commit at each green checkpoint (the controller pushes). Each task gets a read-only critique, and each PR a **Fable** whole-branch review.

## Review Focus

1. **A result scored from a stale or partial attempt.** A retry that dies early must never be scored with the previous attempt's `vpr.out`, and a missing gated metric (`-1`, `0`, absent) must fail the gate rather than drop out of the geomean. Pinned in Task 5 (`test_fresh_attempt_mtime`) and Task 8 (`test_gate_missing_metric`).
2. **A cached job reused after something changed.** Rebuilding `odin3`, editing `lib/vtr.o3lib` or the ABC script, or moving VTR must rerun the affected legs, and nothing else may. Pinned in Task 2 (`test_job_key_inputs`, `test_leg_key_shares_designs`, `test_plan_rekeys_on_lib_and_script`) and Task 6 (`test_run_reuses_cache`).
3. **The baseline's failure reported as Odin III's.** A capped or timed-out P job must make the verdict INCOMPLETE (exit 3) with "rerun it", never FAIL, and a capped job's peak must be its own. Pinned in Task 8 (`test_gate_incomplete_reference`) and Task 5 (`test_capped_no_verdict`).
4. **A ratio at the gate's boundary printed misleadingly.** `1.200` passes, `1.2001` fails and must print as `1.2001` (never `1.200` beside FAIL), and `1.500` per design passes. Pinned in Task 8 (`test_gate_boundaries`) and Task 9 (`test_md_four_decimals_near_bound`).
5. **The machine dying during an unattended night.** `C:` filling (the VHDX never shrinks), memory pressure from another session, or a job overrunning 06:00: the run pauses, stops after 2 h, terminates at the 10 GiB floor, and releases the lock by morning. Pinned in Task 5 (`test_admission_disk_pause_stop_floor`, `test_admission_mem_wait`) and Task 13 (`test_nightly_deadline`).

**Internal risks for the reviewers:** the cgroup walk (point (b); Task 1's `test_verify_scope_ancestor`/`_refusals`) and process-group kills (Task 5's `test_timeout_kills_group`).

---

## PR 3H-1 — harness and the P leg

### Task 1: `qor_heavy` — lock, cap, machine readers, safe deletion (QH-7.1, §7.3 readers, §7.5 `safe_rmtree`; T6, part of T4)
**Files:**
- Create `tools/vtr-qor/qor_heavy.py` and `tests/tools/test_vtr_qor_heavy.py`.
- Modify `pyproject.toml` (`tools/vtr-qor` in `[tool.ruff] src` and `[tool.mypy] mypy_path`) and `tests/tools/__init__.py` (the `sys.path` list) — conflict note 3.

**Interfaces:**
- Consumes: `tools/heavy-run` (2E-1) as an executable only.
- Produces (`qor_heavy.py`):
  - `HEAVY_DIR = Path("/tmp/claude-1000")`, `HEAVY_LOCK = HEAVY_DIR / "odin3-heavy.lock"`, `CAP_BYTES = 6 * 1024**3`, `CAP_XL_BYTES = 9 * 1024**3`.
  - `class HeavyError(Exception)`: `__init__(self, code: int, message: str)`, with attributes `code` (the §9 exit code) and `message` (without the `vtr-qor: ` prefix, which the CLI adds).
  - `@dataclass(frozen=True) class ScopeInfo: path: Path; leaf: Path; memory_max: int; swap_zero: bool`. `path` is the capping cgroup directory, `leaf` the process's own (point (b)).
  - `REPO_ROOT = Path(__file__).resolve().parents[2]`.
  - `def inside_heavy_run(env: Mapping[str, str] | None = None, *, self_cgroup: Path = Path("/proc/self/cgroup"), fd_link: Path = Path("/proc/self/fd/9"), lock: Path = HEAVY_LOCK) -> bool`: point (a)'s evidence (`ODIN3_HEAVY_SLICE` set, the cgroup path under it, fd 9 resolving to `lock`).
  - `def require_heavy(*, xl: bool = False) -> ScopeInfo`: `inside_heavy_run()` and `verify_scope(...)`, else `HeavyError(2, "not inside a heavy run: the controller starts it through tools/heavy-run")`. 3D's tools call it on golden paths.
  - `def reexec_argv(argv: Sequence[str], script: Path, *, xl: bool = False) -> list[str]`.
  - `def reexec_under_lock(argv: Sequence[str], *, script: Path | None = None, xl: bool = False, no_wait: bool = False, lock: Path = HEAVY_LOCK, env: Mapping[str, str] | None = None) -> ScopeInfo`. `script` defaults to `Path(sys.argv[0]).resolve()`, so 3E's tools call `reexec_under_lock(sys.argv[1:])`.
    - inside a heavy run, it only returns `verify_scope(...)`;
    - outside, without `ODIN3_HEAVY_OK=1` in the caller's environment, it raises `HeavyError(2, "heavy runs are the controller's: set ODIN3_HEAVY_OK=1")` (ruling I3);
    - otherwise it probes the lock, runs `reexec_argv` through `subprocess.run` with the caller's environment unchanged, and calls `sys.exit` with the child's status, mapping 77 per point (a).
  - `def verify_scope(limit: int = CAP_BYTES, *, self_cgroup: Path = Path("/proc/self/cgroup"), cgroup_root: Path = Path("/sys/fs/cgroup")) -> ScopeInfo`: point (b). It raises `HeavyError(2, …)`.
  - `def oom_kills(scope: ScopeInfo) -> int`: `oom_kill` from `<path>/memory.events`, 0 when absent.
  - `def scope_peak_mib(scope: ScopeInfo) -> int | None`: from `<leaf>/memory.peak` (point (b)).
  - `def disk_free_gib(env: Mapping[str, str] | None = None) -> int`. It runs `df -B1G --output=avail /mnt/c`, or the argv in `ODIN3_QOR_DF_CMD` (whitespace-split; tests only), and reads the last line. On failure it raises `HeavyError(5, "cannot read free space of /mnt/c")`.
  - `def mem_available_gib(meminfo: Path = Path("/proc/meminfo")) -> float`.
  - `def check_out_dir(out: Path, home: Path) -> Path`: point (m). It returns the resolved path.
  - `def safe_rmtree(path: Path, root: Path) -> None`: it refuses unless `realpath(path)` lies strictly under `realpath(root)` and no component from `root` down is a symlink.
  - `def main(argv: Sequence[str] | None = None) -> int`: the command `python3 -I tools/vtr-qor/qor_heavy.py require-heavy [--xl]`, for 3E's shell tool `blif-roundtrip --vtr`. It exits 0 inside a heavy run with the cap verified, 3 when not inside, and 2 with `vtr-qor: <message>` on stderr when inside but the cap check fails. Tests point it at fake files through `ODIN3_QOR_SELF_CGROUP`, `ODIN3_QOR_CGROUP_ROOT` and `ODIN3_QOR_FD9`.

**Tests (`test_vtr_qor_heavy.py`; every case uses temp files, fake executables and an injected `lock`; nothing touches the real lock or cgroups):**
- **T6.**
  - `test_reexec_argv`: `reexec_argv(["run", "--leg", "P"], S)` with `S = REPO_ROOT / "tools/vtr-qor/vtr_qor.py"` equals `[str(REPO_ROOT / "tools/heavy-run"), sys.executable, "-I", str(S), "run", "--leg", "P"]`. With `xl=True`, `"--xl"` follows the `heavy-run` path.
  - `test_reexec_env` (ruling I3): with `ODIN3_HEAVY_OK=1` in the caller's environment, a fake `tools/heavy-run` dumping its argv and environment exits 0; the environment it saw equals the caller's exactly, and `reexec_under_lock` exits 0. Without the variable → `HeavyError(2, "heavy runs are the controller's: set ODIN3_HEAVY_OK=1")` and the fake never runs. `xl=True` without `ODIN3_HEAVY_XL_OK=1` raises point (d)'s `HeavyError(2, …)` before the fake runs.
  - `test_reexec_inside`: with `ODIN3_HEAVY_SLICE=odin3-heavy.slice`, a fake cgroup path under that slice, a fake fd-9 link to the lock and a passing fake tree, no subprocess starts and a `ScopeInfo` comes back. The variable alone (no fd link, or a cgroup outside the slice) is not inside (finding 27).
  - `test_require_heavy`: outside → `HeavyError(2, "not inside a heavy run: the controller starts it through tools/heavy-run")`.
  - `test_cli_require_heavy`: `main(["require-heavy"])` inside with a passing tree → 0; outside → 3; inside with a failing tree → 2, and stderr holds the `not inside a capped scope` line.
- **The lock.**
  - `test_lock_busy_no_wait`: the test holds the injected lock with `fcntl.flock(LOCK_EX)` → `HeavyError(4, "heavy lock <lock> is busy (--no-wait)")`.
  - `test_lock_wait_message`: the lock is held, then released by a timer thread after 0.2 s. Stderr holds exactly one line `vtr-qor: waiting for the heavy lock <lock>`, and the fake child runs.
  - `test_heavy_run_77`: the fake exits 77 → point (a)'s `HeavyError(2, "cannot apply the memory cap …")`. A fake exiting 3 → `SystemExit(3)`. With `no_wait` and the lock busy, the fake never starts.
- **`verify_scope`.**
  - `test_verify_scope_ancestor` (finding 26): `/proc/self/cgroup` = `0::/user.slice/user-1000.slice/user@1000.service/odin3.slice/odin3-heavy.slice/odin3-heavy-4242-1760000000000000000.scope` (`heavy-run`'s scope name); the scope has `memory.max`/`memory.swap.max` = `max`/`max`, the slice `6442450944`/`0` → `ScopeInfo(path=<…>/odin3-heavy.slice, leaf=<…>/odin3-heavy-4242-….scope, memory_max=6442450944, swap_zero=True)`. The same under a sibling `odin3-heavyxl.slice` at `9663676416` passes only with `limit=CAP_XL_BYTES`.
  - `test_verify_scope_refusals`: no numeric `memory.max` → `HeavyError(2, "not inside a capped scope (memory.max max, memory.swap.max max); run through the heavy wrapper")`; `8589934592` → the same with `memory.max 8589934592`; no `memory.swap.max` of `0` → refused; `limit=CAP_XL_BYTES` with `9663676416` → passes.
  - `test_oom_and_peak`: `memory.events` holding `oom 2\noom_kill 3\n` → 3; no leaf `memory.peak` → `None`; a leaf `memory.peak` of `1073741824` (the slice's at `4294967296`) → 1024.
- **Machine readers.**
  - `test_disk_free`: `ODIN3_QOR_DF_CMD` set to a fake printing `Avail\n  117\n` → 117. A fake exiting 1 → `HeavyError(5, "cannot read free space of /mnt/c")`.
  - `test_mem_available`: `MemAvailable:   19922944 kB` → 19.0.
- **Deletion (T4's `safe_rmtree`).**
  - `test_safe_rmtree_refusals`: a sibling of the root, and the root itself → `HeavyError(2, "refusing to delete <path>: not under <root>")`; a path through a symlinked directory → `HeavyError(2, "refusing to delete <path>: <component> is a symlink")`, the target untouched; a real subdirectory is deleted.
  - `test_out_dir_roots`: `~/odin3-ws/work/vtr-qor` (with `home` faked) and `/tmp/claude-1000/t` are accepted; `/srv/x` → `HeavyError(2, "refusing --out /srv/x: not under ~/odin3-ws/work or /tmp/claude-1000")`.

### Task 2: `qor_legs`, `sets.json`, the two keys and the planner (QH-2, QH-3, §5.3; T5)
**Files:**
- Create `tools/vtr-qor/qor_legs.py`, `tools/vtr-qor/qor_sched.py` (the planning half; Task 5 adds the runner), `tests/vtr/qor/sets.json`, `tests/tools/test_vtr_qor_legs.py` and `tests/tools/test_vtr_qor_sched.py` (key and planner cases).

**Interfaces:**
- Produces (`qor_legs.py`):
  - `VTR_PIN = "3c9a4d23b27d187bd67c79dc01a18ff1ffc21d9b"`.
  - `LEGS = ("P", "O", "Oprime", "Pprime", "R", "Oprov")`.
  - `def normalise_leg(name: str) -> str`: `O'` → `Oprime`, `P'` → `Pprime`. Anything else outside `LEGS` raises `HeavyError(2, "unknown leg '<L>' (P, O, Oprime, Pprime, R, Oprov)")` (E-QH7).
  - `@dataclass(frozen=True) class LegContext: vtr_root: Path; python: Path; arch_xml: Path; odin3: Path; repo: Path; job_dir: Path; k: int; group: str; seed: int = 1; golden: Path | None = None`.
  - `@dataclass(frozen=True) class Command: argv: tuple[str, ...]; cwd: Path; stdout: Path; env: tuple[tuple[str, str], ...]`. stderr goes to the same file.
  - `def common_params(job_dir: Path, group: str, seed: int = 1) -> list[str]`.
  - `def key_params(seed: int = 1) -> list[str]` (point (f)).
  - `def build_commands(leg: str, design: str, ctx: LegContext) -> list[Command]`.
  - `def odin3_script(leg: str, design_v: Path, out_blif: Path, k: int, name_style: str) -> str`.
    - The signature is spec §3.7's and is frozen for 3F.
    - `design_v` is the leg's input: the `.v` for O, O′ and Oprov, the verified golden BLIF for P′ and R.
    - `$LIB` is `<repo>/lib/vtr.o3lib`, with `<repo>` = `Path(__file__).resolve().parents[2]`.
    - For P it raises `ValueError("leg P runs no odin3 script")`.
  - `LEG_PASSES: dict[str, tuple[str, ...]]`:
    - `O`, `Oprov`: `read_techlib read_verilog proc opt flatten wreduce hardmap lower abc write_blif`;
    - `Oprime`: the same without `abc`;
    - `Pprime`: `read_techlib read_blif abc write_blif`;
    - `R`: `read_techlib read_blif write_blif`;
    - `P`: none.
  - `def registered_passes(odin3_help: str) -> frozenset[str]` (point (t)).
  - `def leg_available(leg: str, passes: frozenset[str]) -> str | None`: the first missing pass in `LEG_PASSES` order, else `None`.
  - `def lut_size(vtr_root: Path, python: Path, arch_xml: Path) -> int`: K, read once per run through VTR's own `determine_lut_size` by point (x)'s `-I` snippet (3E Task 7's mechanism, not `PYTHONPATH`). It is 6 for both arches.
- Produces (`qor_sched.py`, planning):
  - `@dataclass(frozen=True) class KeyInputs: leg: str; design: str; arch_base: str; arch_sha256: str; source_sha256: str; vtr_commit: str; params: tuple[str, ...]; odin3_build: str | None; script: str | None; lib_sha256: str | None; abc_script_sha256: str | None`.
  - `def job_key(k: KeyInputs) -> str`: the hex SHA-256 of `json.dumps({…, "schema": 1}, sort_keys=True, separators=(",", ":"))` over every field.
  - `def leg_key(k: KeyInputs) -> str`: the same without `design` and `source_sha256` (point (f)).
  - `def odin3_build_id(odin3: Path, env: Mapping[str, str] | None = None) -> str` (point (q)).
  - `@dataclass(frozen=True) class Job: leg: str; design: str; arch: str; group: str; gating: bool; key: str; key12: str; job_dir: Path; commands: tuple[Command, ...]; cached: bool`.
    - `job_dir` is `<out>/<vtr12>/<arch base>/<leg>/<key12>/<d>`.
    - `arch` is `k6_frac_N10_frac_chain_mem32K_40nm` or `EArch`.
  - `@dataclass(frozen=True) class PlanRequest: legs: tuple[str, ...]; designs: tuple[str, ...]; arch: str; out: Path; vtr_root: Path; odin3: Path; rerun: bool; seed: int = 1`.
  - `def load_sets(path: Path) -> Sets`. `Sets` has `order`, `gate`, `extra`, `smoke`, `group_b` (tuples) and `designs` (a dict of `DesignInfo(rank, group, golden_bytes, vtr_flow_time_s)`).
  - `def select_designs(sets: Sets, set_names: Sequence[str], designs: Sequence[str]) -> tuple[str, ...]`. It returns §2 order whatever the input order. It raises E-QH7 `unknown design '<d>' (see tests/vtr/qor/sets.json)` / `unknown set '<s>' (gate, extra, smoke)`.
  - `def plan_jobs(req: PlanRequest, sets: Sets) -> list[Job]`.
    - Jobs are design-major and leg-minor, legs in the order `P, Pprime, Oprime, O, R, Oprov`.
    - `cached` is true when `<job_dir>/metrics.json` exists with `"status": "ok"`, its `key` equals the job key, and `rerun` is false.

**`sets.json`:** `{"schema": 1, "sizes_from": "odin3-golden 3931cf4", "order": [21], "gate": [19], "extra": ["spree", "arm_core"], "smoke": [5], "group_b": ["LU8PEEng", "bgm", "mcml", "LU32PEEng"], "designs": {"<d>": {"rank": <int|null>, "group": "A"|"B", "golden_bytes": <int>, "vtr_flow_time_s": <float>}}}`, with §2's values verbatim. `order` is by golden size with the extras in place (`spree` after `mkPktMerge`, `arm_core` after `stereovision1`).

**Commands** (spec §3; the tests pin the argv): every VTR command has `cwd` `<J>`, `stdout` `<J>/run/vtr_flow.out` and `PATH=<V>/.venv/bin:<PATH>`. The odin3 legs first run the front end under `/usr/bin/time -v` into `<J>/frontend.log`, then VTR on `<J>/<d>.blif` with `-start vpr` (O, P′, Oprov) or `-start abc` (O′, R).

**Tests:**
- `test_vtr_qor_legs.py`:
  - `test_p_command`: with `V=/v`, `PY=/v/.venv/bin/python`, `J=/w/j`, group A and design `sha`, the argv is exactly `["/v/.venv/bin/python", "/v/vtr_flow/scripts/run_vtr_flow.py", "/v/vtr_flow/benchmarks/verilog/sha.v", "/v/vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml", "-track_memory_usage", "-crit_path_router_iterations", "100", "-temp_dir", "/w/j/run", "-timeout", "7200", "-delete_intermediate_files", "-delete_result_files"]`.
    - `cwd` is `/w/j`, `stdout` is `/w/j/run/vtr_flow.out`, and the `PATH` entry starts `/v/.venv/bin:`.
    - Group B gives `"14400"`; `seed=2` inserts `"--seed", "2"` after `"100"`.
  - `test_odin3_script_texts` (`<r>` = the repo root): O is `read_techlib <r>/lib/vtr.o3lib; read_verilog --nolatches /v/vtr_flow/benchmarks/verilog/sha.v; proc; opt; flatten; wreduce; hardmap; lower; opt -fast; compact; abc --lut 6; write_blif --vtr --lut-k 6 /w/j/sha.blif`; Oprime drops `abc --lut 6; `; Oprov ends `--name-style provenance /w/j/sha.blif`; Pprime is `read_techlib <r>/lib/vtr.o3lib; read_blif <G>; abc --lut 6; write_blif --vtr --lut-k 6 /w/j/sha.blif`; R drops its `abc --lut 6; `; P → `ValueError`.
  - `test_frontend_commands`: O's first argv is `["/usr/bin/time", "-v", "-o", "/w/j/frontend.time", "<odin3>", "--diag-paths", "resolved", "-p", <O script>]` with `stdout` `/w/j/frontend.log`. Its second argv ends `… "/w/j/sha.blif", "<arch>", "-start", "vpr", …`. Oprime's second has `"-start", "abc"`.
  - `test_leg_names`: `"O'"` → `"Oprime"`; `"X"` → E-QH7's exact text.
  - `test_registered_passes`: a help text with lines `  read_blif <file>: …`, `  check: …`, `  --top NAME …` → `{"read_blif", "check"}`.
  - `test_leg_available`: every O pass but `wreduce` → `"wreduce"`.
  - `test_sets_json`: 21 designs in `order` by ascending size; 19 in `gate`; `extra == ("spree", "arm_core")`; `smoke == gate[:5]`; `group_b == ("LU8PEEng", "bgm", "mcml", "LU32PEEng")`; `designs["mcml"].vtr_flow_time_s == 4699.4`; `designs["ch_intrinsics"].golden_bytes == 59473`.
- `test_vtr_qor_sched.py` (T5 and planning):
  - `test_job_key_inputs`: changing any one `KeyInputs` field changes `job_key`. The keys are 64 hex digits, and `key12` is the first 12.
  - `test_leg_key_shares_designs`: two designs of one leg config share `leg_key` and differ in `job_key`. Changing `odin3_build` changes both keys for O and neither for P (P's odin3 fields are `None`).
  - `test_key_excludes_housekeeping`: the job directory and the timeout are not inputs (`key_params()` holds no `-temp_dir` or `-timeout`). A seed change re-keys.
  - `test_build_id`: a fake `ldd` (via the `qor_sched.LDD_CMD` seam) listing `libodin3.so => /x/libodin3.so` and `libodin3_abc.so => /x/libodin3_abc.so`. Changing one byte of either file, or of `odin3`, changes the id.
  - `test_plan_order`: `--designs sha,ch_intrinsics` with legs `O,P` → `[(ch_intrinsics, P), (ch_intrinsics, O), (sha, P), (sha, O)]`.
  - `test_plan_cache`: an `ok` `metrics.json` with the same key → `cached=True`. A different key, `status failed`, or `rerun=True` → `cached=False`.
  - `test_plan_rekeys_on_lib_and_script` (finding 30): in a temp repo copy, editing one byte of `lib/vtr.o3lib`, or of `lib/abc/vtr.abc`, changes `plan_jobs`' key for every O, O′ and P′ job and leaves P's unchanged.

### Task 3: `qor_parse` — VTR's parse, `time -v`, stage costs (QH-4.2–4.4; T1)
**Files:**
- Create `tools/vtr-qor/qor_parse.py` and `tests/tools/test_vtr_qor_parse.py`.
- Create `tests/tools/fixtures/vtr_qor/{stereovision3_P,ch_intrinsics_P,frontend}/`: the logs of §4.2/§4.4 (`abc1.out` for the two-clock `stereovision3`), `parse_results.txt`, `qor_results.txt`, `expected.json`, and a `frontend.time`.
  - They come from two real P runs of Task 2's command (2–3 s of VTR each), which the **controller** captures through `tools/heavy-run` once 2E-1 is on `main` (ruling I4); the implementer commits them, trimmed to the lines the parse configs match.
  - `expected.json` records what VTR's `parse_vtr_flow.py` printed for the **untrimmed** directories.

**Interfaces:**
- Produces:
  - `def parse_commands(vtr_root: Path, python: Path, run_dir: Path, arch_xml: Path, circuit: str) -> list[Command]`. There are two, exactly:
    - `[<PY>, <V>/vtr_flow/scripts/python_libs/vtr/parse_vtr_flow.py, <run_dir>, <V>/vtr_flow/parse/parse_config/vpr_standard.txt, arch=<arch>.xml, circuit=<circuit>, script_params=common]` with `env` `PYTHONPATH=<V>/vtr_flow/scripts/python_libs` and `stdout` `<run_dir>/parse_results.txt`;
    - the same with `qor_config/qor_standard.txt` into `qor_results.txt`.
  - `def read_parse_results(path: Path, *, not_before: float, label: str) -> dict[str, str]`: one header line, one value line. A file whose mtime is earlier than `not_before` raises `HeavyError(1, "<label>: stale <name> from an earlier attempt; job failed")` (E-QH8; `label` = `<L> <d>`).
  - `def present(text: str | None) -> float | None`: a finite number other than `-1`, else `None`.
  - `@dataclass(frozen=True) class TimeV: wall_s: float | None; peak_rss_mib: int | None`.
  - `def read_time_v(path: Path) -> TimeV`. `Elapsed (wall clock) time (h:mm:ss or m:ss): …` accepts `m:ss.ss` and `h:mm:ss`; `Maximum resident set size (kbytes): N` gives `(N + 512) // 1024`.
  - `def vpr_time_s(run_dir: Path) -> float | None`: the sum of every `The entire flow of VPR took (.*) seconds` in `vpr.out` and `vpr.crit_path.out`.
  - `@dataclass(frozen=True) class StageCost: wall_s: float; peak_rss_mib: int; complete: bool; warnings: tuple[str, ...]`.
  - `STAGE_GLOBS = ("parmys.out", "abc*.out", "*_blackboxing_latch.out", "restore_latch*.out", "report_clocks.abc.out")`.
  - `def stage_costs(run_dir: Path, globs: Sequence[str], label: str) -> StageCost`: wall times summed, peak the maximum. A matched log without `time -v` lines sets `complete=False` and adds `vtr-qor: <label>: no time -v in <file>; stage cost incomplete`.
  - `def metrics_from_parse(parse: Mapping[str, str], qor: Mapping[str, str]) -> dict[str, dict[str, float | int | str | None]]`. It fills §5.2's `packed`, `routed` and `cost.max_vpr_mem_mib` / `vpr_total_runtime_s` by §4.2's table:
    - `fmax_mhz = 1000 / critical_path_delay_ns` when present;
    - `max_vpr_mem_mib = max_vpr_mem / 1024`, rounded half up;
    - an absent or `-1` value is `None`.

**Tests (T1):**
- `test_fixture_values`: for each fixture, `metrics_from_parse` of the committed `parse_results.txt`/`qor_results.txt` equals `expected.json`'s mapped values exactly (`num_clb`, `min_chan_width`, `critical_path_delay_ns`, `crit_path_routed_wirelength`, `device_width`, `device_height`, `num_memories`, `num_mult`, `max_vpr_mem_mib`, `vpr_status`), and `vpr_time_s` equals the sum recorded there.
- `test_parse_commands`: the two argv lists exactly as above for `circuit=sha.v`.
- `test_stale_parse`: a `parse_results.txt` whose mtime is set (`os.utime`) one second before `not_before` → `HeavyError(1, "P sha: stale parse_results.txt from an earlier attempt; job failed")`.
- `test_present`: `"-1"`, `""`, `"nan"`, `"inf"`, `None` → `None`; `"0"` → `0.0`; `"1.934"` → `1.934`.
- `test_time_v`: `0:14.23` → 14.23; `1:02:03.5` → 3723.5; `Maximum resident set size (kbytes): 70000` → 68. A file without the lines → `TimeV(None, None)`.
- `test_stage_costs_two_clock`: on `stereovision3_P`, `abc0.out` and `abc1.out` and the helper logs are counted (`complete=True`; wall = the fixture's recorded sum). Deleting the `time -v` lines from `abc1.out` in a temp copy gives `complete=False` and the exact warning.
- `test_missing_key`: dropping `min_chan_width` from a copy → `routed.min_chan_width is None`.

### Task 4: `qor_netstats` — counts, depth and the `# abc:` header (QH-4.1; T2)
**Files:**
- Create `tools/vtr-qor/qor_netstats.py`, `tests/tools/test_vtr_qor_netstats.py`, and `tests/tools/fixtures/vtr_qor/netstats/*.blif` with an `expected.json`.

**Interfaces:**
- Produces:
  - `@dataclass(frozen=True) class NetStats: luts: int; buffers: int; ffs: int; adders: int; mults: int; ram_slices: int; depth: int; blif_sha256: str`.
  - `def netstats(path: Path) -> NetStats`.
    - It reads `.blif` or `.blif.gz` (`blif_sha256` is of the uncompressed bytes).
    - Only the first `.model` counts: the stubs after its `.end` are ignored.
    - It joins `\` continuations, counts in one pass, then runs Kahn's algorithm over the LUT graph. There is no recursion, and memory is O(nets).
    - On a loop it returns `depth = -1` and prints `vtr-qor: netstats: <path>: combinational loop; depth -1` to stderr.
  - `@dataclass(frozen=True) class AbcHeader: script: str; sha256: str; abc: str; k: str; exchange: str; depth: int`.
  - `def abc_header(path: Path) -> AbcHeader | None`.
    - It returns the last `# abc: ` line before `.model`, parsed as blank-separated `key=value` words. A value may be in double quotes with `\"` and `\\` (3D §8.5).
    - It returns `None` when there is none.
    - A line missing `depth=` or `exchange=` raises `ValueError("<path>: '# abc:' line without depth= or exchange=")`.
- Rules, exactly §4.1:
  - `luts` counts `.names` with ≥ 1 input, excluding identity buffers (one input whose only row is `1 1` or `0 0`);
  - `buffers` counts those identity buffers;
  - zero-input `.names` are constant generators and count nowhere;
  - `ffs` counts `.latch` lines;
  - `adders`, `mults` and `ram_slices` count `.subckt adder`, `.subckt multiply`, and `.subckt single_port_ram` + `.subckt dual_port_ram`;
  - depth counts levels of non-buffer LUTs. Sources are primary inputs, latch outputs, hard-block outputs and constants; sinks are primary outputs, latch inputs and hard-block inputs. A buffer adds 0 levels.

**Tests (T2):**
- `test_counts`: `mixed.blif` (hand-written: 2 `.inputs`, 1 `.latch`, 6 `.names` of which one is `1 1`, one is `0 0` and one has zero inputs, an adder, a multiply and a RAM, plus stubs after `.end`) → `NetStats(luts=3, buffers=2, ffs=1, adders=1, mults=1, ram_slices=1, …)`. `blif_sha256` is the file's `sha256sum`.
- `test_depth_chain`: a chain of three LUTs fed by a PI → 3. Through a buffer → still 3. A `multiply` in the middle cuts the path, so the depth is the longer side (2).
- `test_depth_latch_cut`: a LUT → latch → LUT path → 1.
- `test_loop`: two LUTs feeding each other → −1 and the stderr line exact.
- `test_continuations_and_gz`: the same netlist with `\` continuations and gzipped gives an identical `NetStats`.
- `test_abc_print_stats`: `abc_small.blif` is a subckt-free, single-clock netlist that ABC (`third_party/abc`'s `abc`, at the pin) mapped, committed with the `lev = N` its `print_stats` printed as a `# lev = N` comment. `depth == N`.
- `test_abc_header`:
  - two lines `# abc: script=builtin:lib/abc/vtr.abc sha256=<64 hex> abc=<40 hex> K=6 exchange=blif depth=4` and `… exchange=aig depth=7` → `exchange == "aig"`, `depth == 7`;
  - `script="my dir/x.abc"` → `script == "my dir/x.abc"`;
  - no line → `None`.

### Task 5: `qor_sched` — the runner (QH-7.2–7.6; T4)
**Files:**
- Modify `tools/vtr-qor/qor_sched.py`.
- Modify `tests/tools/test_vtr_qor_sched.py`; create `tests/tools/fixtures/vtr_qor/fakejob/fakejob.py`, a scripted fake job driven by a JSON spec: sleep, exit code, files to write with their mtimes, a child to spawn, `oom_kill` increments to a fake `memory.events`.

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces:
  - The Global Constraints' scheduler caps as named constants (`MAX_JOBS` … `KEEP_FAILED_BYTES`).
  - `KEEP_RUN_FILES`: §7.5's keep list exactly (`vtr_flow.out output.txt parse_results.txt qor_results.txt vpr.out vpr.crit_path.out parmys.out` and the globs `abc*.out`, `*_blackboxing_latch.out`, `restore_latch*.out`, `report_clocks.abc.out`).
  - `@dataclass class RunOptions: jobs: int = 1; job_timeout_s: Mapping[str, int] = JOB_TIMEOUT_S; keep_netlists: frozenset[str] = frozenset(); fcheck: bool = False; deadline: float | None = None; scope: ScopeInfo | None = None; clock: Callable[[], float] = time.time; sleep: Callable[[float], None] = time.sleep; env: Mapping[str, str] = os.environ; meminfo: Path = Path("/proc/meminfo"); out: Path; expected_s: Callable[[Job], float] | None = None`.
  - `@dataclass(frozen=True) class JobResult: job: Job; status: str; elapsed_s: float; error: str`. `status` is one of `ok failed capped timeout skipped aborted`.
  - `@dataclass(frozen=True) class RunSummary: results: tuple[JobResult, ...]; stopped_for_disk: bool; exit_code: int`. The exit code follows the Global Constraints.
  - `def run_jobs(jobs: Sequence[Job], opts: RunOptions, finish: Callable[[Job, float], dict[str, object]]) -> RunSummary`.
    - Every job runs alone, group A then group B, in plan order (`opts.jobs` must be 1; E-QH9 above it).
    - With `opts.fcheck`, each job's fcheck (Task 11) runs right after its VPR job, before the next job starts.
    - `finish(job, attempt_start)` builds `metrics.json` (Task 6) and is called for a job whose commands all exited 0.
  - `def fresh_attempt(job: Job) -> float`: it deletes `metrics.json` and `status`, runs `safe_rmtree(<J>/run)`, recreates `<J>/run`, and returns the start time.
  - `def run_one(job: Job, opts: RunOptions, finish: …) -> JobResult`. It runs each `Command` with `start_new_session=True` under the job wall timeout, reads `oom_kills` before and after, and classifies:
    - a failure with a risen counter is `capped`;
    - an expired timeout is `timeout`: SIGTERM to the process group, SIGKILL after `KILL_GRACE_S`;
    - any other nonzero exit is `failed`.
  - `def admit(opts: RunOptions, *, running: int) -> str`. It returns `"go"`, `"wait"` or `"stop"` and prints the §7.3 messages. Disk free comes from `disk_free_gib(opts.env)`.
  - `def cleanup_job(job: Job, result: JobResult, keep_netlists: frozenset[str]) -> None` (§7.5).
  - `def prune(out: Path, vtr12: str, arch: str, leg: str) -> None` (§5.4).
  - `class HarnessLog`: it appends to `<out>/harness.log` and echoes to stderr. Its lines are `vtr-qor: [<i>/<n>] <leg> <design> <status> <elapsed> s (<group>, j<k>)` (elapsed with one decimal) and, at the end, `vtr-qor: done: <ok> ok, <failed> failed, <capped> capped, <timeout> timeout, <skipped> skipped in <h:mm:ss>`.
- Behaviour:
  - Admission re-checks every `POLL_S` with §7.3's texts (`paused: …`, `resumed: …`, `waiting: <x.y> GiB available (< 7)`), and E-QH11's after `PAUSE_LIMIT_S` (plus the compaction hint line) or below `DISK_FLOOR_GIB` (running jobs `aborted`), exit 5.
  - Capped: E-QH10's line (point (k)), then `vtr-qor: <L> <d>: no verdict`. Timeout: `vtr-qor: <L> <d>: timeout after <s> s`.
  - Each job writes `<J>/status` (one word) when it ends. Retention runs after the whole run.

**Tests (T4; fake jobs, a fake `df` via `ODIN3_QOR_DF_CMD` reading a file the test rewrites, a fake meminfo, a fake `memory.events`, an injected clock and sleep):**
- `test_order_groups`: five A and two B jobs, each writing start/end stamps, with fcheck on: no two jobs (or a job and another job's fcheck) overlap, each fcheck follows its own VPR job, and every B start follows the last A end.
- `test_j_cap`: `jobs=2` → `HeavyError(2, "-j 2 exceeds 1 (the heavy-run rule)")` (E-QH9).
- `test_capped_no_verdict`: a job exits 1 after incrementing `oom_kill`, with its leaf `memory.peak` at 6,000 MiB and the slice's at 6,100 → `capped`, the E-QH10 line with `peak ≥ 6000 MiB`, then `no verdict`, no retry, exit 3. A failure without a risen counter → `failed`, exit 1.
- `test_fresh_attempt_mtime`: `fresh_attempt` removes a previous attempt's `run/vpr.out` and `run/parse_results.txt`; a fake job that exits 0 but leaves a `parse_results.txt` dated one second before the attempt start → `failed`, `vtr-qor: P sha: stale parse_results.txt from an earlier attempt; job failed`.
- `test_timeout_kills_group`: a fake job that spawns a grandchild sleeping 600 s, with a 1 s job timeout → `timeout`. Neither PID is alive after `KILL_GRACE_S` (injected as 0.5 s), and the log line is `vtr-qor: P sha: timeout after 1 s`.
- `test_admission_disk_pause_stop_floor`: df 15 → the exact paused line, then df 25 → the resumed line, and the job runs; df 15 for 7,200 s of fake clock → the stop line with `(3 jobs not started)`, the hint line, exit 5; df 9 while one job runs → `terminating 1 running jobs`, that job `aborted`, exit 5.
- `test_admission_mem_wait`: `MemAvailable` 6.5 GiB → `vtr-qor: waiting: 6.5 GiB available (< 7)`. Raising it to 8 admits the job.
- `test_cleanup_keep_list`: after an ok P job whose `run/` held 20 files, `run/` holds exactly the keep list present plus `sha.pre-vpr.blif.gz` (gunzips to the original). An O job loses `run/sha.pre-vpr.blif`, and `<J>/sha.blif` becomes `<J>/sha.blif.sha256` (hex digest and `\n`); with `keep_netlists={"O"}` both stay. Failed and capped jobs keep `run/` whole.
- `test_retention`: five complete keys → the three newest remain; seven failed directories → five, oldest pruned first; three failed 1 GiB (sparse) directories → ≤ 2 GiB; a symlink inside a fake `out` makes `prune` raise E-QH12 rather than follow it.
- `test_progress_lines`: the per-job line and the `done:` line, character for character.
- `test_failure_texts`: a front-end command exiting 3 → `failed`, `vtr-qor: O sha: front end exited 3; see <J>/frontend.log`. A VTR command exiting 0 whose parsed `vpr_status` is `failed` → `failed`, `vtr-qor: P sha: vpr_status=failed; see <J>/run/vtr_flow.out` (E-QH8).

### Task 6: the CLI — `doctor`, `run`, `status`, `clean`; `metrics.json`; T8 (QH-9, §5.2, §12 step 1)
**Files:**
- Create `tools/vtr-qor/vtr-qor` (the bash shim), `tools/vtr-qor/vtr_qor.py` and `tests/tools/test_vtr_qor_cli.py`.

**Interfaces:**
- Consumes: Tasks 1–5.
- Produces:
  - `def main(argv: Sequence[str] | None = None) -> int`. It catches `HeavyError`, prints `vtr-qor: <message>`, and returns its code.
  - The common options are spec §9's with its defaults (`--out` checked per point (m)), plus `--xl` on `run --designs` only (point (d)).
  - Subcommands here: `doctor`, `run`, `status`, `clean`. Tasks 9 and 11–15 add the rest of §9.
  - `run` takes §9's options. It checks VTR (E-QH1, E-QH2), selects designs (E-QH7), checks legs (E-QH5), and plans. `--dry-run` prints `<leg> <design> <key12> cached|run` per job and stops (not heavy); otherwise it calls `reexec_under_lock`, then `run_jobs`.
  - `def check_vtr(vtr_root: Path, allow_drift: bool) -> str` returns the full commit, raising E-QH1/E-QH2 (the dirty check is `git status --porcelain --untracked-files=no`).
  - `def finish_job(job: Job, attempt_start: float) -> dict[str, object]`, per completed job: Task 3's parse commands, `netstats` of `run/<d>.pre-vpr.blif`, `abc_header` of `<J>/<d>.blif` for O, P′ and Oprov (point (e)), `frontend.time` or the stage costs; then, atomically, `metrics.json` (schema 1, §5.2's keys plus `netlist.abc_exchange`) and `job.json` (leg, design, every key input and command argv, `PATH` and `ODIN3_*`, UTC start/end, host `{cpu, mem_gib}`, slice, `vtr_drift`, and for P point (l)'s fields); and `latest` (atomic rename) once the leg key is complete.
  - `doctor` (not heavy) prints `ok|FAIL  <item>: <detail>` per check and exits 0 when all pass, else 1. The checks: the VTR pin and a clean tracked tree; `vpr`, `abc` and `yosys` as VTR's `vtr.paths` names them; `verilator` on `PATH`, the venv, `/usr/bin/time -v true`; `tools/heavy-run` (`FAIL  heavy-run: tools/heavy-run missing (2E-1 not merged)`) and the probe `systemd-run --user --scope -q --slice=odin3-heavy.slice true`; `/mnt/c` free GiB, `MemAvailable`, the lock (non-blocking probe); and `legs: <available>; <L> missing '<pass>'` from `odin3 --help`.
  - `status`: per (arch, leg), one line `<arch> <leg> latest <key12|-> ok <n> failed <m> age <d>d`.
  - `clean [--keep N]`: Task 5's `prune` over every (vtr12, arch, leg).

**Tests (`test_vtr_qor_cli.py`, with fakes for VTR (a temp git repo at a chosen commit), `odin3 --help` and the heavy wrapper):**
- `test_dry_run_deterministic`: `run --leg P --set smoke --dry-run` twice gives identical stdout of 5 lines in §2 order (T7, part).
- `test_vtr_errors`: a missing directory → exit 2 and the E-QH1 text. A repo at another commit → the E-QH2 text with both `sha12`s. One modified tracked file → `… has 1 modified tracked files`. `--allow-vtr-drift` proceeds, and `job.json` records `"vtr_drift": true`.
- `test_unknown_inputs`: `--designs nope` → exit 2 and E-QH7's design text. `--set big` → its set text.
- `test_leg_unavailable`: a help text without `wreduce` and `--leg O` → exit 2, `vtr-qor: leg O needs pass 'wreduce', which <odin3> does not register` (E-QH5).
- `test_run_reuses_cache`: with a fake `run_vtr_flow.py`, a second `run` of the same design makes no job start (`harness.log` has no new start line). `--rerun` runs it again. Changing the fake `odin3` binary's bytes re-runs only O-type legs.
- `test_metrics_schema`: a finished fake P job's `metrics.json` has exactly §5.2's key tree plus `netlist.abc_exchange`, with `"schema": 1` and `"gating": true` for a gate design and `false` for `spree`.
- `test_isolated_import`: `python3 -I tools/vtr-qor/vtr_qor.py --help` exits 0 (point (u)). The shim `tools/vtr-qor/vtr-qor --help` gives the same output.
- `test_doctor_checklist`: with every fake present → exit 0. With `tools/heavy-run` removed → exit 1, with the exact FAIL line.

**T8 (local, after the unit tests pass, once 2E-1 is on `main`; point (g)):** `ODIN3_HEAVY_OK=1 tools/vtr-qor/vtr-qor run --leg P --designs ch_intrinsics,stereovision3,diffeq2`, run by the controller (or by the implementer only when the controller sets `ODIN3_HEAVY_OK=1` for that session).
- 3 jobs end `ok` with `vpr_status=success`.
- Each `metrics.json`'s routed values equal its `parse_results.txt`.
- `run/` holds exactly the keep list plus the `.pre-vpr.blif.gz`.
- Quote the `done:` line in the PR.

## PR 3H-run-P — the first P run (controller)

### Task 7: the Parmys baseline on the 19 + 2 (QH-12; T10's data)
**Files:** none in the repo. The results stay under `~/odin3-ws/work/vtr-qor/`. Task 9 commits the PHASE3 line and figures (`main` is protected and this run has no branch).

**Steps (a Sonnet `high` run session; spec §12):**
1. `tools/build-locked --preset release -j6`.
2. `tools/vtr-qor/vtr-qor doctor`. Expected on 2026-10-11: VTR `3c9a4d23b27d` clean, the binaries and venv present, `C:` ≈ 117 GiB free, `MemAvailable` ≈ 19 GiB, the lock free. Stop on any FAIL.
3. Start `tools/vtr-qor/vtr-qor run --arch k6 --leg P --set gate,extra` in the background and watch `harness.log` with Monitor until `vtr-qor: done:`. The controller exports `ODIN3_HEAVY_OK=1`. Group A is 17 jobs, one at a time (≈ 1,185 s on VTR's runner, ≈ 30 min × 1.5); then `LU8PEEng`, `bgm`, `mcml` and `LU32PEEng` (≈ 9, 9, 78 and 54 min). Plan for **≈ 4–4.5 h** solo (point (w); ruling C1), with nothing else heavy.
4. Acceptance (§12 item 5): 21 jobs `ok` with `vpr_status=success`, exit 0. A capped or timed-out job goes to Peter with its E-QH10 line and peak; the cap is not raised.
5. Record the first measurement of the ABC stage's peak RSS: every `abc*.out` peak for all 21, `mcml` and `LU32PEEng` called out (§7.1 step 1).
6. T2's local check: `depth` against VTR's `abc_depth` for each single-clock design. Mismatches are explained in 3H-2's PR description.
7. Note `parmys_equals_golden` per design (point (l)).

The run's `metrics.json` files are the record until 3H-2 renders the table from them, without rerunning anything (spec §12 item 7).

## PR 3H-2 — the table and the gate

### Task 8: ratios, geomean, the gate (QH-6.1, 6.2; T3)
**Files:**
- Create `tools/vtr-qor/qor_table.py` (the verdict half) and `tests/tools/test_vtr_qor_table.py`.
- Create `tests/vtr/qor/bands.json`, exactly §6.2's JSON.
- Create `tests/vtr/qor/explanations.tsv`: only the header comment `# design<TAB>metric<TAB>text (a row explains why O != P for num_mult or num_memories)`. Lines starting `#` are comments everywhere in these TSVs.

**Interfaces:**
- Produces:
  - `PAIRS = (("O", "P"), ("Oprime", "P"), ("Pprime", "P"), ("O", "Oprime"), ("R", "P"), ("Oprov", "O"))`.
  - `@dataclass(frozen=True) class Run: leg: str; design: str; arch: str; key12: str; status: str; gating: bool; metrics: Mapping[str, object]`.
  - `def load_runs(out: Path, vtr12: str, arch: str, keys: Mapping[str, str]) -> list[Run]`. `keys` maps leg → `key12`; a leg not given reads `latest`. It raises E-QH13 `table: no complete key for leg <L> on <arch>` (exit 1).
  - `def metric(run: Run, name: str) -> float | None`. It finds `name` in `netlist`, `packed`, `routed`, `cost` or `verdicts`, through `present`.
  - `def ratio(x: float | None, y: float | None, gated: bool) -> float | None`:
    - gated: defined only when both are present and > 0;
    - report-only: when both are present, x ≥ 0 and y > 0; and 1.0 when both are 0.
  - `def geomean(values: Sequence[float]) -> float | None`: `exp(mean(log r))` over the defined values; `None` when empty.
  - `@dataclass(frozen=True) class Bands: pair: tuple[str, str]; set: str; metrics: tuple[str, ...]; geomean_max: float; per_design_max: float; equal_or_explained: tuple[str, ...]`.
  - `def load_bands(path: Path) -> Bands`.
  - `def load_tsv(path: Path) -> list[tuple[str, str, str]]`: the comment lines skipped, three fields per line, else `ValueError("<path>:<line>: expected 3 tab-separated fields")`.
  - `@dataclass(frozen=True) class Verdict: state: str; violations: tuple[str, ...]`. `state` is `PASS`, `FAIL` or `INCOMPLETE`.
  - `@dataclass(frozen=True) class GateInputs: runs: Sequence[Run]; bands: Bands; gate_set: Sequence[str]; explanations: Sequence[tuple[str, str, str]]; divergences: Sequence[tuple[str, str, str]]; equiv_exceptions: Sequence[tuple[str, str, str]]`.
  - `def gate(inp: GateInputs) -> Verdict`. It applies §6.2's rules 1–7 in order, compares unrounded values, and collects every violation.
    - INCOMPLETE wins over FAIL only when a reference job (P, or P′/O′ for the attribution rows) is not `ok`, or a P fcheck is `error`, or point (p) applies. A table without O is INCOMPLETE with `gate: INCOMPLETE: no O leg in the table`.
    - The violation texts are E-QH13/E-QH14's exactly, with `<x>` formatted by `fmt_ratio`.
  - `def fmt_ratio(value: float, bounds: Sequence[float]) -> str`: 3 decimals, or 4 when `abs(value − b) < 0.005` for any bound `b`.

**Tests (T3; synthetic `Run` lists built in the test):**
- `test_ratio_rules`: gated `(0, 5)`, `(5, 0)`, `(None, 5)` → `None`; report-only `(0, 0)` → 1.0, `(0, 5)` → 0.0, `(5, 0)` → `None`.
- `test_geomean`: `[1.1, 1.2, 1.3]` → `1.1972` to 4 places; a set with one undefined ratio reports `n = 18` of 19.
- `test_gate_boundaries`: every design at 1.20 → PASS; a `num_clb` geomean of exactly 1.2001 → `gate: FAIL: num_clb geomean O/P 1.2001 > 1.20`; per design 1.500 → PASS, 1.5001 → `gate: FAIL: num_clb sha O/P 1.5001 > 1.50`.
- `test_gate_missing_metric`: O's `min_chan_width` `-1`, `0` or absent for `sha` → FAIL with `gate: FAIL: min_chan_width missing for sha (O)`. P's value missing gives the same text with `(P)`.
- `test_gate_missing_design`: no O run for `bgm` → `table: bgm has no ok O job` among the violations, and FAIL.
- `test_gate_incomplete_reference`: P's `mcml` with status `capped` → INCOMPLETE with `gate: INCOMPLETE: mcml P is capped (reference); rerun it`. Any other violation is still listed.
- `test_gate_equal_or_explained`: `num_mult` O 5 vs P 4 for `sha` → `gate: FAIL: num_mult sha O 5 != P 4 (no row in explanations.tsv)`. A row `sha\tnum_mult\t…` → PASS.
- `test_gate_fcheck`: O `fail` without a row → `gate: FAIL: sha O fcheck fail (no row in divergences.tsv)`; with `mem-init-dropped` → PASS; `baseline-fails` → FAIL when P passes, PASS when P fails; O `error` → `gate: FAIL: sha O fcheck error`; P `error` → INCOMPLETE.
- `test_gate_equiv`: `differs`, even on a listed design → `gate: FAIL: sha equiv differs`; `timeout` on `sha` → `gate: FAIL: sha equiv timeout (not in equiv-exceptions.tsv)`; on a listed `stereovision3` → PASS (rule 5 stands in); `ram` → PASS.
- `test_gate_rerun_and_status`: `rerun differs` → `gate: FAIL: sha rerun differs`. O's `vpr_status=failed` → `gate: FAIL: sha O vpr_status=failed`.
- `test_bands_json`: `load_bands` of the committed file → `pair ("O", "P")`, `geomean_max 1.20`, `per_design_max 1.50`, the four metrics, `equal_or_explained ("num_mult", "num_memories")`.

### Task 9: writers, `table`/`gate`/`publish`, the P-baseline table (QH-6.3, 6.4; T3 goldens, T7)
**Files:**
- Modify `tools/vtr-qor/qor_table.py` (writers) and `tools/vtr-qor/vtr_qor.py` (`table`, `gate`, `publish`).
- Create `tests/tools/fixtures/vtr_qor/table/{runs/…,expected/qor.md,expected/runs.csv,expected/ratios.csv,expected/runs.json}`. The inputs are a synthetic job tree of P, O′, P′ and O for 19 + 2 designs, with hand-computed geomeans recorded in a `README`.
- Modify `tests/tools/test_vtr_qor_table.py`.
- Modify `docs/PHASE3.md`:
  - the checklist line `- [x] 3H: Parmys baseline (P) on the 19, -temp_dir ~/odin3-ws/work/vtr-qor, table generator (run <date>, key <key12>, <h:mm> wall, published qor/<vtr12>/P-baseline)`;
  - an entry with Task 7's measured wall time and ABC-stage peaks, the geomeans of `num_clb`, min W, CPD and wirelength against VTR's golden, and the `parmys_equals_golden` list;
  - decision 17's agent-default note (spec §19).
- Modify `docs/ORACLES.md`: `vtr-qor doctor` is the VTR-pin check.

**Interfaces:**
- Produces:
  - `@dataclass(frozen=True) class TableHeader: arch: str; label: str; vtr12: str; odin3_commit12: str | None; build12: str | None; abc_sha12: str | None; legs: Mapping[str, str]; host: str; date_utc: str`.
    - `host` and `date_utc` come from the newest job's `job.json` end time and its host record (spec §6), never from the clock.
  - `def write_md(runs: Sequence[Run], verdict: Verdict | None, header: TableHeader, path: Path, *, golden: GoldenXcheck | None) -> None`: §6.3's sections and columns in order; the gate section first, omitted without O; Netlist and Attribution name each P′/O row's `abc_exchange`; Cost marks `*` and `xl` and states §18 item 2's comparability caveat; a "Cross-check against VTR's golden" section whenever P is present.
  - `def write_csv(runs: Sequence[Run], runs_path: Path, ratios_path: Path) -> None`: §6.4's headers as one line each; pairs `O/P`, `O'/P`, `P'/P`, `O/O'`, `R/P`, `Oprov/O`; one `GEOMEAN_GATE` and one `GEOMEAN_EXTRA` row per pair, `n` = the count of defined ratios; §6's precisions.
  - `def write_json(runs: Sequence[Run], verdict: Verdict | None, header: TableHeader, path: Path) -> None`: the `metrics.json` objects, `"header"`, and `"gate": {"pass": bool, "violations": [str]}`.
  - `@dataclass(frozen=True) class GoldenXcheck: values: Mapping[tuple[str, str], float]; bands: Mapping[str, tuple[float, float]]`.
  - `def load_golden_xcheck(vtr_root: Path) -> GoldenXcheck` reads `vtr_flow/tasks/regression_tests/vtr_reg_nightly_test3/vtr_reg_qor_chain/config/golden_results.txt` and the `pass_requirements_file=` its `config.txt` names. A metric outside its band is listed `outside` and fails nothing (PHASE3 exit item 5).
  - CLI:
    - `table --legs L,… [--keys L=key12,…] --label TEXT [--set …]` writes `<out>/tables/<YYYYMMDD-HHMM of the header date>-<label>/{qor.md,runs.csv,ratios.csv,runs.json}` and prints the directory;
    - `gate <table dir>` prints the verdict line, then the violations, and exits 0, 1 or 3;
    - `publish <table dir>` is refused without `ODIN3_QOR_PUBLISH=1` (E-QH15, exit 2). Otherwise it copies the four files to `~/odin3-ws/golden/qor/<vtr12>/<label>/` and prints `git -C ~/odin3-ws/golden add qor/<vtr12>/<label>` and `git -C ~/odin3-ws/golden commit -m "qor: <label> (<vtr12>)"`.

**Tests:**
- `test_table_golden_files`: the synthetic tree gives `qor.md`, `runs.csv`, `ratios.csv` and `runs.json` byte-equal to `expected/`.
- `test_table_twice_identical` (T7): two `table` runs over the same tree give identical bytes in all four files, even with the files' mtimes and the clock changed.
- `test_md_four_decimals_near_bound`: a geomean of 1.2004 prints `1.2004` (not `1.200`) in `qor.md` and `ratios.csv`; 1.150 prints `1.150`.
- `test_csv_headers`: the first lines equal §6.4's two headers exactly.
- `test_gate_cli`: the exit codes for PASS, FAIL and INCOMPLETE trees are 0, 1 and 3, and the first stdout line is `Gate (decision 3): PASS|FAIL|INCOMPLETE`.
- `test_publish`: without the variable → exit 2 and E-QH15's text. With it and `HOME` faked → the four files under `<HOME>/odin3-ws/golden/qor/3c9a4d23b27d/<label>/` and the two `git` lines exactly.
- `test_p_only_table`: `--legs P` renders the P columns and the cross-check section with no gate section, and `gate` on it exits 3 with `gate: INCOMPLETE: no O leg in the table`.

**Close-out (controller):**
1. `vtr-qor table --legs P --label P-baseline` over Task 7's jobs, then `publish`.
2. Commit the odin3-golden README section `qor/<vtr12>/<label>/` (the four files; their schema; "written by `vtr-qor publish`").
3. Commit the PHASE3 line and entry above in this PR.

## PR 3H-3 — control legs, functional and structural checks, nightly, M1

### Task 10: legs P′ and R, the golden check, depth and exchange (QH-3.4, 3.5, §4.1 `abc_depth`)
**Files:**
- Modify `tools/vtr-qor/qor_legs.py`, `qor_sched.py` and `vtr_qor.py`.
- Modify `tests/tools/test_vtr_qor_legs.py` and `test_vtr_qor_cli.py`.

**Interfaces:**
- Produces:
  - `def golden_path(arch_base: str, design: str, home: Path) -> Path`: `<home>/odin3-ws/golden/<arch base>/vtr/<d>/<d>.parmys.blif`.
  - `def verify_golden(blif: Path, arch_xml_rel: str) -> str | None`. It reads `<d>.parmys.prov` (`key=value` lines) and returns `None` when the golden is usable, else the reason:
    - `missing`;
    - `status=<s>`;
    - `vtr_commit <a> is not the pin <b>`;
    - `arch <a> is not <b>` (against `vtr_flow/arch/timing/<arch>.xml`);
    - `blif_sha256 does not match`.
  - `plan_jobs` gives a P′ or R job whose golden fails `skipped` status, with E-QH6 `<d>: Parmys golden <path>: <reason>` (counted as exit 1).
  - For P′ and R the design-source key input is the golden BLIF's SHA-256.
  - `finish_job` for P′: `cost.frontend_wall_s` holds `frontend.time` only. The table adds the Parmys part per point (o), and shows both parts.

**Tests:**
- `test_verify_golden`: one temp `.prov` per reason, each giving its exact text. A good one → `None`. The real `ch_intrinsics.parmys.prov` (read-only, skipped when absent) → `None`.
- `test_pprime_skipped`: a bad golden → status `skipped`, `metrics.json` `"status": "skipped"` with `error` = the E-QH6 text, and run exit 1.
- `test_pprime_commands`: the script reads the golden path; the VTR argv has `-start vpr`. R has `-start abc` and no `abc` in the script.
- `test_abc_header_metrics`: a fake P′ whose `<d>.blif` carries `# abc: … exchange=blif depth=5` → `netlist.abc_depth == 5`, `netlist.abc_exchange == "blif"`. Without the line, both are `null`.

### Task 11: `fcheck` — the Verilator random check (QH-8.1; decision 16a)
**Files:**
- Modify `tools/vtr-qor/qor_sched.py` (the inline group A step and the separate group B step) and `vtr_qor.py` (`fcheck`; `run --fcheck`).
- Create `tests/vtr/qor/divergences.tsv`. Its first row is `ch_intrinsics\tmem-init-dropped\tdecision 6 step 5: hardmap drops RAM initial values (3C)`. Later `baseline-fails` rows each cite P's `fcheck.log` path and the reason (Sonnet `high` research, one row per design found).
- Modify `tests/tools/test_vtr_qor_sched.py` and `docs/ORACLES.md` (`run_random_check.py` is the functional oracle).

**Interfaces:**
- Produces:
  - `def fcheck_command(ctx: LegContext, design: str, netlist: Path, verilator_j: int) -> Command`: exactly `[<PY>, <V>/mosaic/verilator_check/run_random_check.py, --rtl, <V>/vtr_flow/benchmarks/verilog/<d>.v, --post-synth-blif, <netlist>, --work-dir, <J>/fcheck, --vectors, 50000, --seed, 1, --verilator-j, <n>]`, with `cwd` `<J>` and `stdout` `<J>/fcheck.log`.
  - `def fcheck_verdict(returncode: int, capped: bool) -> str`: 0 → `pass`, 1 → `fail`, 2 → `error`, capped → `capped`, anything else → `error`.
  - `def run_fcheck(job: Job, opts: RunOptions, verilator_j: int) -> str`:
    - for P it gunzips `run/<d>.pre-vpr.blif.gz` into `<J>/fcheck/<d>.pre-vpr.blif`;
    - for other legs it needs the kept `run/<d>.pre-vpr.blif`, else `vtr-qor: fcheck: <L> <d>: no kept netlist (run with --keep-netlists)` and `error`;
    - it rewrites `metrics.json` `verdicts.fcheck` atomically;
    - it deletes `<J>/fcheck` on `pass` and keeps it otherwise.
  - Scheduling (ruling C1): with `--fcheck`, every job runs its fcheck right after its own VPR job and before the next job starts, never beside another job: `--verilator-j 2` in group A, `1` in group B.
  - `fcheck --leg L [--set|--designs]` runs the step on existing jobs.

**Tests:**
- `test_fcheck_command`: the argv exactly, for `--verilator-j 2`.
- `test_fcheck_exit_mapping`: a fake script exiting 0, 1 and 2 gives `pass`, `fail` and `error`. One that increments `oom_kill` and exits 137 gives `capped`.
- `test_fcheck_p_from_gz`: the fake receives `<J>/fcheck/sha.pre-vpr.blif`, equal to the gunzipped kept file.
- `test_fcheck_dir_kept_on_fail`: `<J>/fcheck` is gone after `pass` and present after `fail`.
- `test_fcheck_after_own_job`: each fcheck starts after its own VPR job ends and ends before the next job starts; B fchecks receive `--verilator-j 1`.
- `test_divergences_rows`: every committed row parses with `load_tsv`, and its class is one of `mem-init-dropped`, `async-reset-made-sync`, `baseline-fails`.

**The group B measurement (a run session; §7.1 "what is not yet measured", step 2):** `vtr-qor fcheck --leg P --designs LU8PEEng`, then `bgm`, then `mcml`, then `LU32PEEng`. Each runs solo under the lock, and the peaks are recorded in 3H-3's PR description. A capped one goes to Peter with its peak (point (d)).

### Task 12: `equiv` — `equiv-check` before and after `abc` (QH-8.2; decision 16a)
**Files:**
- Modify `tools/vtr-qor/vtr_qor.py` and `qor_sched.py`.
- Create `tests/vtr/qor/equiv-exceptions.tsv` with six rows:
  - `stereovision3\tmulticlock\ttwo clocks (tm3_clk_v0, tm3_clk_v2): the Verilator check stands in`;
  - and `diffeq1`, `diffeq2`, `stereovision1`, `stereovision2`, `bgm`, each `\tmultiplier\t<n> multiply, inlined by equiv-check as 36x36 logic: the Verilator check stands in`, with n = 5, 5, 152, 468, 11.
- Modify `tests/tools/test_vtr_qor_sched.py`.

**Interfaces:**
- Produces:
  - `EQUIV_TIMEOUT_S = 600`.
  - `def equiv_command(repo: Path, before: Path, after: Path) -> list[str]`: exactly `[<repo>/tools/equiv-check/equiv-check, --timeout, 600, <before>, <after>]`.
    - `before` is the O′ job's kept `<d>.blif`; `after` is the O job's.
  - `def equiv_verdict(returncode: int | None, stderr: str) -> str`:
    - 0 → `equivalent`; 1 → `differs`;
    - 2 with `unsupported: more than one clock domain` → `multiclock`;
    - 2 with `running ABC failed:` and `timed out` → `timeout`;
    - `None` (the harness's 660 s wall cap expired) → `timeout`;
    - else `error`.
  - `def run_equiv(design: str, o_job: Job, oprime_job: Job) -> str`.
    - A netlist with `ram_slices > 0` (from `metrics.json`) → `ram`, not run.
    - The verdict is written to the O job's `metrics.json` `verdicts.equiv`.
    - `equiv` is always heavy (finding 28): it goes through `reexec_under_lock`, since `equiv-check` runs ABC's `dsec`.
  - `equiv [--set|--designs]`.

**Tests:**
- `test_equiv_command`: the argv exactly.
- `test_equiv_classification`: fake `equiv-check` scripts for each exit and stderr case give the six verdicts.
- `test_equiv_ram_skipped`: an O job with `ram_slices` 4 → `ram`, and no subprocess starts.
- `test_equiv_exceptions_file`: the six committed rows parse and name gate designs. `sha`, `stereovision0` and `blob_merge` are absent. The `stereovision0` row with class `timeout` is added only by the reviewed PR after its M3 measurement (spec §8.2).

### Task 13: `nightly`, `check-lib`, `crosscheck`, the EArch smoke, and the M1 run (QH-13, §9, T9, T11)
**Files:**
- Modify `tools/vtr-qor/vtr_qor.py` and `qor_sched.py`.
- Create `tools/vtr-qor/systemd/odin3-vtr-qor-nightly.service` and `.timer`, and `tests/tools/test_vtr_qor_nightly.py`.
- Modify `docs/PHASE3.md` (the M1 checklist tick with the run's summary).

**Interfaces:**
- Produces:
  - `nightly [--budget H] [--slot N]` (point (c); ruling I2): started inside `tools/heavy-run` by the controller, it checks `require_heavy()` and runs §13's steps 1–7 at once, in one held lock. The inner subcommands run in-process; a heavy child tool (3E's and 3D's) gets `pass_fds=(9,)`.
  - `def nightly_deadline(start: datetime, budget_h: float) -> datetime`: the earlier of the next 06:00 local after the start and the start plus the budget (default 5 h).
  - `def expected_minutes(job: Job, history: Path, sets: Sets) -> float`. It is the last measured `elapsed_s` of the same (arch, leg, design) from `metrics.json` under any key. Else it is `vtr_flow_time_s` × 1.5, plus the leg's last `frontend_wall_s` when the leg runs `odin3`.
  - Admission: before each job, a job whose expected end passes the deadline is not started. The log gets `vtr-qor: nightly: <L> <d> not started: expected <m> min would pass 06:00`, exit 0 for that line alone (E-QH10). Running jobs finish.
  - `ROTATION`: §13 step 4's eight `(design, legs)` slots: 1 `LU8PEEng` O O′ P′ (+ fcheck O); 2 `bgm` O O′ P′ (+ fcheck O, equiv); 3–5 `mcml` O, O′, P′; 6–8 `LU32PEEng` O, O′, P′.
    - State: `<out>/nightly/rotation.json` = `{"next_slot": n, "last_admitted": "<date>", "starved": {"<slot>": k}, "last_sweep_commit": "<sha>", "last_sweep_date": "<date>", "last_3d_commit": "<sha>"}`.
    - A slot advances only when it ran. Unavailable legs are skipped; a slot with none available advances with an info line.
  - Starvation guard (§13): a slot refused the night before runs step 4 before step 3; after two refused nights `summary.md` starts with the starved line and group A runs only legs whose odin3 build id changed.
  - Steps:
    1. `doctor`, `vtr-models check --arch <arch>`, and `check-lib`.
    2. The EArch smoke: `run --arch earch --set smoke --leg <every available leg>`.
    3. k6 group A: every available leg, with `--fcheck` on O, `--keep-netlists` for O and O′ (point (n)), then `equiv` and `rerun-check` (Task 14; skipped until it lands).
    4. One rotation slot. P on group B runs only when VTR moved, as a night of its own.
    5. The 3E sweeps, as 3E-4's plan defines them (`blif-roundtrip --vtr` and `vtr-read-check` on the k6 golden tree), when `git -C <repo> log <last_sweep_commit>..HEAD -- src/backends/blif src/frontends/blif` is non-empty or on a Saturday night. A sweep night skips step 4 without advancing the rotation. Before 3E-4 merges: `vtr-qor: nightly: 3E sweeps skipped (tools/vtr-read-check absent)`.
    6. **The 3D sweeps** (ruling I10; review finding 10): 3D's `abc-equiv` on the k6 goldens except `mcml` and `LU32PEEng` (3D §13.4), `abc-diff` on the same set (§13.5), and `abc-determinism --mode a` and `--mode c` on the same set (§13.8 (a), (c)), plus `--mode d` (Debug against Release, needing `build/debug` and `build/release`) on the micros only, as 3D §13.8 (d) says: the committed BLIFs and the k6 `regression/` micro goldens, never `LU8PEEng`, `bgm` or any `vtr/` design (re-review N1). All run as direct commands with `pass_fds=(9,)`, smallest first. They run when `git log <last_3d_commit>..HEAD -- src/passes/abc adapters/abc` is non-empty, else on Sunday nights; a 3D-sweep night also skips step 4. Before 3D-5 merges: `vtr-qor: nightly: 3D sweeps skipped (tools/abc-check absent)`. Each tool's summary goes into `summary.md`.
    7. `table --label nightly-<YYYYMMDD>`, then `nightly/<YYYYMMDD>/summary.md`, listing every (leg, design, gated metric) that moved more than 2 % from the previous nightly table, and every changed status.
  - The unit files: `.service` (`Type=exec`, `Environment=ODIN3_HEAVY_OK=1`, `ExecStart=%h/odin3-ws/odin3/tools/heavy-run %h/odin3-ws/odin3/tools/vtr-qor/vtr-qor nightly`) and `.timer` (`OnCalendar=*-*-* 01:00`, `Persistent=false`, `WantedBy=timers.target`), with §13's install and remove commands in a header comment. No agent installs them.
  - `check-lib`: it runs `[<repo>/tools/vtr-models/vtr-models, check-lib, <repo>/lib/vtr.o3lib, --models, <repo>/tests/vtr/arch/k6_frac_N10_frac_chain_mem32K_40nm.models.json, --vtr-root, <V>]` and exits with its status (3E §15.2).
  - `crosscheck --task DIR` (heavy) runs `[<PY>, <V>/vtr_flow/scripts/run_vtr_task.py, <abs DIR>, -temp_dir, <out>/crosscheck, -j, 1]` on the three smallest designs. Every non-time key of each `parse_results.txt` must equal the harness's value (time keys: names ending `_time` or `_runtime`, or starting `max_vpr_mem`), else `vtr-qor: crosscheck: <d> <key>: task <a>, harness <b>`, exit 1 (point (h)).

**Tests (`test_vtr_qor_nightly.py`; fake clock, fake jobs):**
- `test_nightly_deadline`: started at 01:00 with a fake job list whose expected minutes are 120, 120, 100 and 90 → the first two start; the third is refused at 05:00 with the exact E-QH10 nightly text; the fourth, a 30-minute job, still starts. Started at 14:00 with `--budget 4` → deadline 18:00.
- `test_nightly_requires_heavy_run`: outside a heavy run → exit 2 with `require_heavy`'s message, and no step runs (no Python-side locking, no delay).
- `test_3d_sweep_night`: a changed `src/passes/abc/run.c` → step 6 runs the five 3D commands in that order, each with descriptor 9 passed, and step 4 is skipped.
- `test_rotation_advance_and_starvation`: slot 3 admitted → `next_slot` 4; slot 4 refused → the next night runs step 4 before step 3; refused twice → `summary.md` starts `vtr-qor: nightly: group B slot 4 (mcml O′) starved for 2 nights`.
- `test_sweep_night`: a changed `src/backends/blif/writer.c` in a temp git repo → step 5 runs and step 4 is skipped with the rotation unchanged. No change and not Saturday → step 5 is skipped.
- `test_summary_diff`: a `num_clb` move of 2.5 % and a status change `ok` → `failed` are listed; a 1.9 % move is not.
- `test_unit_files`: the files hold the lines listed above.
- `test_check_lib_argv`, `test_crosscheck_compare`: the argv exactly. A fake task `parse_results.txt` differing in `num_clb` gives the exact crosscheck line and exit 1; differing only in `vpr_flow_runtime` gives exit 0.

**The M1 run (a run session; T11; PHASE3 "M1: P′ leg routes all 19"):**
1. `tools/build-locked --preset release -j6`, then `vtr-qor run --leg Pprime --set gate,extra --keep-netlists` in the background, watched with Monitor.
2. Acceptance: all 19 `ok` with `vpr_status=success`; the `ch_intrinsics` P′ `--vtr` file keeps every `addr[5..14]=unconn` on each RAM slice (3E §14.1 item 2, pinned in 3D by `test_abc_unconn`), counted with `grep -c` and quoted in the PR; `qor.md`'s P′/P rows name the exchange (point (e)).
3. The `odin3`+ABC group B measurement (§7.1 step 3): `LU8PEEng`, `bgm`, `mcml`, `LU32PEEng`, each alone; their `frontend.time` peaks go into the PHASE3 M1 entry, and a capped one to Peter (point (d)).
4. `vtr-qor table --legs P,Pprime --label M1`; the controller publishes it.

## PR 3H-4 — the exit

### Task 14: legs O′, O and Oprov; `rerun-check`; the M3 seed reruns (QH-3.2, 3.3, 3.6, §8.3, §6.3 seeds)
**Files:**
- Modify `tools/vtr-qor/qor_legs.py`, `qor_sched.py`, `vtr_qor.py` and `qor_table.py` (the seed columns, the Name style section).
- Modify `tests/tools/test_vtr_qor_legs.py`, `test_vtr_qor_cli.py` and `test_vtr_qor_table.py`.

**Interfaces:**
- Produces:
  - `build_commands` for O, Oprime and Oprov is already defined by Task 2. Here the legs become runnable: `leg_available` gates them, and `nightly`/`exit-run` skip an unavailable leg with `vtr-qor: <L> skipped: needs pass '<p>'`.
  - `rerun-check --leg O [--set|--designs]`:
    - it runs the O front end again (the first `Command` of the job, with its output path replaced by `<J>/rerun.blif`) in a fresh process;
    - it compares `sha256(<J>/rerun.blif)` with `<J>/<d>.blif.sha256`, or with the kept file's SHA-256;
    - it writes `verdicts.rerun` `same` or `differs` into the O job's `metrics.json`, and deletes `rerun.blif`.
  - `run --seed N` passes `--seed N` through `run_vtr_flow` (a new key, point (f)).
  - `def seed_candidates(runs: Sequence[Run], bands: Bands) -> list[str]`: the gate designs whose O/P ratio of any gated metric is ≥ 1.35, plus, when a metric's geomean is ≥ 1.08, the designs whose ratio for that metric is above that geomean.
  - `qor.md` shows, beside each candidate's single-seed gated value, the three-seed mean as `mean(s1..s3)`. The gate stays on seed 1.
  - `qor.md`'s Name style section: VPR time and memory of Oprov over O, per design and as geomeans.

**Tests:**
- `test_rerun_check`: a fake `odin3` writing identical bytes → `same`; writing one byte different → `differs`. `rerun.blif` is deleted either way.
- `test_seed_candidates`: synthetic ratios: 1.36 for `sha` → listed; a geomean of 1.09 with `bgm` at 1.12 → listed; everything below → empty.
- `test_seed_key`: `--seed 2` changes the key; seed 1 matches Task 2's key.
- `test_leg_skip_message`: `exit-run` with O unavailable → the exact skip line; the other legs run.
- `test_md_seed_and_name_style`: the expected `qor.md` fragment for one candidate with seeds 1–3, and one Name style row, byte-equal.

### Task 15: `exit-run`, the M2 and M3 runs, the exit table (QH-9 `exit-run`, §12 exit, PHASE3 exit items 1–5)
**Files:**
- Modify `tools/vtr-qor/vtr_qor.py` and `tests/tools/test_vtr_qor_cli.py`.
- Modify `docs/PHASE3.md`:
  - the M2 tick and entry;
  - the M3/exit tick with the gate table verbatim, the seed spread, the `equiv` exceptions as measured (the `stereovision0` row if it came), and points (a)–(x) of this plan as agent defaults;
  - decision 17's note if Task 9 did not land it.
- Modify `docs/DESIGN.md` §13 #6: answered with the Oprov numbers.
- Modify `tests/vtr/qor/equiv-exceptions.tsv` and `divergences.tsv` only with measured, reviewed rows.

**Interfaces:**
- Produces `exit-run --label TEXT [--budget H]`, heavy, with no `--xl` (point (d)) and no delayed start (point (c)). It shares `nightly`'s admission deadline and `expected_minutes` (Task 13), and runs in order:
  1. `run --leg P,Oprime,Pprime,O --set gate,extra --fcheck --keep-netlists`, each fcheck right after its own VPR job;
  2. P fcheck from the kept netlists;
  3. `equiv`;
  4. `rerun-check --leg O --set gate`;
  5. `run --arch earch --set smoke --leg P,Pprime,Oprime,O`;
  6. the seed reruns for `seed_candidates`, seeds 2 and 3, O and P;
  7. `table --legs P,Oprime,Pprime,O --label <label>`;
  8. `gate`.

  Re-running `exit-run` with the same label resumes: cached jobs are skipped and each verdict already in `metrics.json` is kept. It prints `vtr-qor: exit-run: step <k>/8 <name>` before each step, and exits with `gate`'s code when every job ran, else with the run's code.

**Tests:**
- `test_exit_run_steps`: with fakes, the eight steps run in order with the exact argv each builds. A second invocation after a simulated stop at step 3 starts no job of steps 1–2 (cache) and resumes at step 3.
- `test_exit_run_exit_code`: a fake gate FAIL → exit 1. A capped `mcml` P → exit 3.
- `test_exit_run_deadline`: started at 01:00 on a fake clock, a job expected to pass 06:00 is not started (the nightly E-QH10 text with `exit-run` in place of `nightly`), and the next invocation resumes there.

**The runs (Sonnet `high` sessions; background + Monitor):**
- **M2:** `vtr-qor run --leg Oprime --set gate,extra --keep-netlists`, after 3A/3B/3C/3G and the 2D–2F passes are registered. All 19 must route. The table `--legs P,Oprime --label M2` goes into the PHASE3 M2 entry, and the controller publishes it.
- **M3, the exit:** `vtr-qor exit-run --label exit-<YYYYMMDD>`.
  - It takes ≈ 20–30 h, about 1 h more with group A serial (point (w)): three legs on 21 designs ≈ 13–14 h, fcheck ≈ 3–6 h, `rerun-check` ≈ 3–4 h, `equiv` ≈ 1–1.5 h, the smoke ≈ 0.3 h, seeds 0–6+ h.
  - It runs over 5–7 nights of ≈ 5 h. Each night is hand-started as in point (c), with `exit-run --label <same>` in place of `nightly`, and resumes through the cache.
  - The first O run measures `stereovision0`'s `equiv`. A `timeout` adds its `equiv-exceptions.tsv` row with the measured time in a reviewed PR (spec §8.2).
  - Acceptance: `gate` PASS (T12). The gate table goes into PHASE3 verbatim, and the seed spread goes to Peter with it. INCOMPLETE reruns the named jobs. FAIL goes to Peter with the violations; no band is relaxed.
  - DESIGN §13 #6 is answered from the Name style section (one Oprov run, at M3).
