# Phase 7 (release) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** v1.0: a flow script and user docs that take a basic design through, the core support the
reverse-engineering tool needs (`submod`, design copy, `cone_check`, `raise`), and a source plus
pip release.

**Architecture:** the flow script is a stdlib Python driver over the `odin3` CLI that reuses the
vtr-qor leg script (one definition of the flow); the docs are Sphinx/MyST built in CI. The RE
support is four core features: three passes (every IR mutation goes through a pass, so `check`
runs around it) and one ABI function (design copy). The release adds version, install, tarball,
a scikit-build-core wheel and a clean-machine test.

**Tech Stack:** C17 core (`src/passes`, `src/api`), linked ABC `cec`, Python 3.10+ stdlib tools, cffi
binding (`plugins/python/odin3.py`), Sphinx + myst-parser (docs only), scikit-build-core (wheel).

**Spec:** `docs/specs/2026-10-21-7-release-design.md` (sections cited as spec 2.1, 3.1, ...).

## Global Constraints

- Code standard of `CLAUDE.md`: C17, functions <= 60 lines / CCN <= 15 / <= 5 parameters, no
  recursion in IR traversals, `odin3_status` errors, IR objects in module storage, `odin3_` ABI
  prefix, every public function documented (purpose, ownership, failure modes).
- Tools are stdlib-only Python, `ruff check` and `mypy --strict` clean, tests with `unittest` in
  `tests/tools/`. Sphinx, myst-parser and scikit-build-core are dev/release dependencies only.
- One pass per PR; each pass has a unit test, a golden or equivalence test and a `docs/PASSES.md` row.
- No flag guessed: every `odin3` command and option comes from `docs/PASSES.md`, `odin3 help`,
  `tools/vtr-qor/qor_legs.py` or an existing CTest.
- Heavy runs (VPR, sweeps) only under `tools/heavy-run` (`ODIN3_HEAVY_OK=1`), `sweep` label, not in CI.
- No API stability promise (spec 0, decision 6); no CIRCT, EDIF (spec 5).
- `raise` (Task 8) needs Peter's review before merge (mapping-style algorithm).
- Version string `1.0.0` (spec 4, P-1).

## Review Focus

1. `submod` on a group whose boundary net has two drivers or an inout bit: a located error, never a
   silently wrong port direction (Task 5, test `submod_multi_driver_refused`).
2. `cone_check` with a binding that leaves a template bit unbound or binds one twice: a located error
   before any proof (Task 7, test `cone_check_binding_incomplete`).
3. `raise` after the group changed since the proof (a node added, a binding edited): refused
   (Task 8, test `raise_refuses_stale_proof`).
4. `odin3_flow.py` with a design whose top is not given and the file holds two modules: usage error 2
   naming the modules (Task 1, test `flow_requires_top_when_ambiguous`).
5. The wheel's binding when both a bundled library and `$ODIN3_LIB` exist: the bundled one wins, and
   `odin3.find_library()` says which it chose (Task 11, test `wheel_prefers_bundled_lib`).

---

## Lane A (7.3): flow script and user docs

### Task 1: `odin3_flow.py` core (targets `vtr` without VPR, `lpm`)

**Files:**
- Create: `tools/flow/odin3_flow.py`, `tools/flow/odin3-flow` (sh wrapper, as `tools/prov-cover/prov-cover`)
- Test: `tests/tools/test_odin3_flow.py`; CTests `flow_vtr_<micro>`, `flow_lpm_<micro>` in `CMakeLists.txt`
- Modify: `pyproject.toml` (ruff/mypy paths, as prov-cover was added)

**Interfaces:**
- Consumes: `tools/vtr-qor/qor_legs.py`: `odin3_script(leg: str, design_v: Path, out_blif: Path, k: int, name_style: str) -> str`, `lut_size(vtr_root, python, arch_xml) -> int`, `arch_xml_path(vtr_root, arch) -> Path` (leg `"O"`, name style `"short"`).
- Produces: `run_flow(args: FlowArgs) -> FlowResult`; `FlowArgs` dataclass (designs: list[Path], top: str | None, target: str, arch: Path | None, family: str, vpr: bool, analysis: bool, odin3: Path, out: Path); `FlowResult` (ok: bool, steps: list[StepResult], summary: dict[str, object]); `StepResult` (name: str, ok: bool, log: Path, seconds: float); `main(argv) -> int` (0 ok, 1 step failed, 2 usage).

- [ ] Step 1: tests: `test_vtr_no_vpr_writes_blif` (micro `bm_DL_logic_w_Dff.v`, top given: `out/odin3/<top>.blif` exists, exit 0); `test_lpm_writes_verilog_and_lints` (an LPM micro from the 5.1 tests: LPM Verilog written, read-back step ok); `test_failing_step_names_step_and_log` (a syntax-error design: exit 1, message contains `step odin3` and the log path); `flow_requires_top_when_ambiguous` (two modules, no `--top`: exit 2, both names in the message); `test_lpm_flow_matches_5_1` (the LPM command list equals the one the 5.1 CTest runs; read it from CMakeLists, do not retype).
- [ ] Step 2: run, expect failures (module missing).
- [ ] Step 3: implement: one subdirectory per step under `--out`; the `vtr` script from `odin3_script("O", ...)`; LUT size from the arch via `lut_size` (when `$VTR_ROOT` is unset and no arch given: k = 6, stated in the log); the `lpm` steps as the 5.1/5.2 CTests run them.
- [ ] Step 4: run tests and the new CTests; lint.
- [ ] Step 5: commit and push.

### Task 2: VPR step and summary

**Files:** Modify `tools/flow/odin3_flow.py`; Test `tests/tools/test_odin3_flow.py`; CTest `flow_vpr_sweep` (sweep label, `single_ff`, `ch_intrinsics`).

**Interfaces:**
- Consumes: Task 1 `run_flow`; vtr-qor's VPR command builder (`build_commands` / `common_params` in `qor_legs.py`: reuse, do not copy) and `qor_parse.py` for the VPR log.
- Produces: `summary.json` keys `luts, clbs, hard_blocks (dict type -> count), min_chan_width, crit_path_ns, wirelength, seconds (dict step -> float)`.

- [ ] Step 1: tests: `test_summary_from_vpr_logs` (fixture VPR logs from an existing vtr-qor test: the keys and values parsed); `test_no_vpr_without_vtr_root` (env without `VTR_ROOT`: VPR skipped with a NOTE, exit 0).
- [ ] Step 2: run, expect failures.
- [ ] Step 3: implement VPR pack/place/route at minimum channel width with vtr-qor's options; print the summary table.
- [ ] Step 4: tests; `ODIN3_HEAVY_OK=1 ctest -R flow_vpr_sweep` in Release passes on both designs.
- [ ] Step 5: commit and push.

### Task 3: `--analysis`

**Files:** Modify `tools/flow/odin3_flow.py`; Test `tests/tools/test_odin3_flow.py`.

- [ ] Step 1: test `test_analysis_outputs` (micro: `analysis/` holds `<top>.dot`, `<top>.odin3.json`, `levelize.txt`, `locs.txt`; each non-empty; the JSON loads with `plugins/python/odin3_graph.py`).
- [ ] Step 2: run, expect failure.
- [ ] Step 3: implement with the options in `docs/PASSES.md` (`write_dot --color-by`, `--locs`; `write_json --odin3`; `levelize`; `dump_locs --types`).
- [ ] Step 4: tests pass; lint.
- [ ] Step 5: commit and push.

### Task 4: user docs (Sphinx + MyST) and CI job

**Files:**
- Create: `docs/user/conf.py`, `docs/user/index.md`, `getting-started.md`, `flow.md`, `odin3-flow.md`, `passes.md` (generated include), `inputs.md`, `outputs.md`, `analysis.md`, `python-api.md`, `docs/user/_ext/passes_ref.py` (reads `docs/PASSES.md` and `odin3 help` output at build time), `requirements-docs.txt` (sphinx, myst-parser, pinned versions).
- Modify: `.github/workflows/ci.yml` (new job `docs`: install `requirements-docs.txt`, `sphinx-build -W -b html docs/user build/docs`; add `docs` to the `ci` job's `needs`).
- Test: `tests/tools/test_passes_ref.py` (the extension lists every pass row of `docs/PASSES.md`).

- [ ] Step 1: test `test_every_pass_listed` (count of generated entries == rows in `docs/PASSES.md`).
- [ ] Step 2: run, expect failure.
- [ ] Step 3: write the extension and the pages (spec 2.2 list; the flow page has one diagram per stage, Graphviz or Mermaid through a Sphinx extension in `requirements-docs.txt`; getting-started uses `odin3_flow.py` on one micro, commands copied from a run).
- [ ] Step 4: `sphinx-build -W` passes locally; the test passes; lint.
- [ ] Step 5: commit and push. (The RE workflow page is written in Task 9.)

## Lane B (7.4a/b): `submod` and design copy

### Task 5: `submod` pass (spec 3.1)

**Files:**
- Create: `src/passes/submod/submod.c`, `submod.h`, `register.c` (layout as `src/passes/flatten/`)
- Modify: `src/passes/builtin.c` (pass list, near `&odin3_pass_levelize` at line ~852), `CMakeLists.txt`, `docs/PASSES.md` (row)
- Test: `tests/unit/test_submod.c`; CTest `submod_equiv_micros`

**Interfaces:**
- Produces: pass `submod [-attr KEY] [-name N]` (default KEY `submod`); `extern const odin3_pass odin3_pass_submod;`. Also `odin3_status odin3_submod_extract(odin3_design *d, odin3_module_id m, const char *key, const char *value, odin3_module_id *out)` in `submod.h` (one group to a new module plus instance; used by Task 7). New module name = attribute value (or N); instance name `<V>_i`; port names = real net names else `p<k>`.
- Consumes: IR module/node/net creation and node move as `flatten` uses them (in reverse); provenance `DERIVED` record helper as `flatten/prov.c`.

- [ ] Step 1: unit tests: `submod_two_gates` (2 marked of 4 gates: new module with the right inputs/outputs, one instance, `check` clean); `submod_ports_from_boundary` (S-2 cases: driven outside -> input; read outside -> output; module output read -> output); `submod_moves_prov_and_attrs`; `submod_name_clash_refused`; `submod_port_node_refused`; `submod_empty_group_refused`; `submod_multi_driver_refused` (Review Focus 1).
- [ ] Step 2: run, expect failures.
- [ ] Step 3: implement: collect groups by attribute value (sorted by value for determinism), compute boundary nets with a worklist (no recursion), create module + ports, move nodes, create instance, record `DERIVED` provenance.
- [ ] Step 4: unit tests pass; CTest `submod_equiv_micros`: for each default micro, mark every second node by ID (`-attr submod` via a small test script), `submod`, `flatten`, `equiv-check` against the input: all equivalent; ASan clean; lint.
- [ ] Step 5: commit and push.

### Task 6: `odin3_design_copy` and `Design.copy()` (spec 3.2)

**Files:**
- Modify: `include/odin3/odin3.h` (declaration + paragraph), `src/api/design.c`, `plugins/python/odin3.py` (`Design.copy()`), `docs/PASSES.md`/API docs as applicable
- Test: `tests/unit/test_design_copy.c`, `tests/tools/test_python_copy.py` (cffi venv, as `test_graph_export`)

**Interfaces:**
- Produces: `odin3_status odin3_design_copy(const odin3_design *src, odin3_design **out);` caller frees with `odin3_design_destroy`; `ODIN3_ERR_NO_MEMORY` with nothing left allocated. Python `Design.copy() -> Design`.

- [ ] Step 1: tests: `copy_json_identical` (`write_json` of src and copy equal byte for byte); `copy_is_independent` (run `opt` on the copy: src's JSON unchanged); `copy_keeps_ids` (a node ID looked up in both gives the same name and type); `copy_oom_leaves_nothing` (allocation-failure hook if the util layer has one; else document why not testable); Python `test_copy_independent`.
- [ ] Step 2: run, expect failures.
- [ ] Step 3: implement as a deep copy of module storage (paged arrays, arenas), attributes, provenance and srcman; IDs preserved.
- [ ] Step 4: tests pass; ASan/leak clean; lint.
- [ ] Step 5: commit and push.

## Lane C (7.4c/d): `cone_check` and `raise`

### Task 7: `cone_check` pass (spec 3.3)

**Files:** Create `src/passes/cone_check/cone_check.c`, `bind.c`, `register.c`; Modify `src/passes/builtin.c`, `CMakeLists.txt`, `docs/PASSES.md`; Test `tests/unit/test_cone_check.c`, CTest `cone_check_micros`.

**Interfaces:**
- Consumes: Task 5's group extraction (factor it in Task 5 as `odin3_submod_extract(odin3_design*, odin3_module_id, const char *key, const char *value, odin3_module_id *out)` in `src/passes/submod/submod.h` so this task reuses it); the linked ABC `cec` entry used by `abc_equiv` tests (find it in `adapters/abc`).
- Produces: pass `cone_check -attr KEY=V -cell TYPE [-param P=V ...] [-timeout S]` (default 60 s); module attributes `odin3:cone_check` (`equivalent|differs|unproven`), `odin3:cone_key` (`KEY=V`), `odin3:cone_hash` (hex SHA-256 of sorted node IDs and sorted `net:binding` pairs), `odin3:cone_cex` (on differs: `name=bit,...`). Helper `odin3_cone_hash(const odin3_module*, const char *key, const char *value, char out[65])` exported for Task 8.

- [ ] Step 1: tests: `cone_check_ripple_adder_equivalent` (4-bit ripple of full adders vs `$add` WIDTH=4); `cone_check_swapped_binding_differs` (A[0] and A[1] swapped: addition is commutative in A/B, so swap two bits of one operand; `differs` with a cex); `cone_check_timeout_unproven` (`-timeout 0`); `cone_check_register_refused`; `cone_check_binding_incomplete` (Review Focus 2: one bit unbound, one bound twice: located errors); `cone_check_hash_stable` (same group and binding, two runs: same hash).
- [ ] Step 2: run, expect failures.
- [ ] Step 3: implement: extract group to a temp module, build the template cell in another, `lower` both to gates, map ports through the binding, ABC `cec` with the limit, write attributes, remove temp modules.
- [ ] Step 4: tests pass; `cone_check_micros` CTest; ASan clean; lint.
- [ ] Step 5: commit and push.

### Task 8: `raise` pass (spec 3.4; Peter reviews the PR)

**Files:** Create `src/passes/raise/raise.c`, `register.c`; Modify `src/passes/builtin.c`, `CMakeLists.txt`, `docs/PASSES.md`; Test `tests/unit/test_raise.c`, CTest `raise_micros`.

**Interfaces:**
- Consumes: Task 7's attributes and `odin3_cone_hash`.
- Produces: pass `raise -attr KEY=V -cell TYPE [-param P=V ...]`.

- [ ] Step 1: tests: `raise_replaces_proven_group` (after `cone_check` equivalent: one `$add` node, group nodes gone, `check` clean, equivalent to the input); `raise_provenance_union` (the new cell's sources == union of the removed nodes' sources); `raise_refuses_unproven`; `raise_refuses_stale_proof` (Review Focus 3: add a node to the group after the proof).
- [ ] Step 2: run, expect failures.
- [ ] Step 3: implement: verify attributes and hash, create the cell, connect through the binding, delete group nodes and internal nets, `DERIVED` provenance with the union of sources.
- [ ] Step 4: tests pass; ASan clean; lint.
- [ ] Step 5: commit and push. PR description asks Peter's review.

## Lane D (7.4e): example, review, docs

### Task 9: `re_round.py`, ABI fitness review, RE docs

**Files:** Create `plugins/python/re_round.py`, `docs/user/re-workflow.md`; Modify `docs/RE-ARCHITECTURE.md` section 5; Test `tests/tools/test_re_round.py`, CTest `python_re_round`.

- [ ] Step 1: test `test_re_round_finds_add` (input: an adder micro through `abc` written as BLIF with generated names; after the round the written Verilog contains `$add` and `equiv-check` against the input is equivalent).
- [ ] Step 2: run, expect failure.
- [ ] Step 3: implement the round: read BLIF, guess the adder cone with a simple structural rule (XOR/majority pattern per bit chained by carry), set `submod`-style attributes and `odin3:bind`, run `cone_check`, `raise`, `write_verilog`; using only the public ABI and `odin3.py`.
- [ ] Step 4: dispatch a read-only review agent over the ABI and bindings against `docs/RE-ARCHITECTURE.md` section 2; fix small gaps here, write large ones into section 5; write `re-workflow.md`.
- [ ] Step 5: tests pass; docs build `-W`; commit and push.

## Lane E (7.6): release

### Task 10: version, install, tarball

**Files:** Modify `CMakeLists.txt` (project VERSION 1.0.0, `install()` rules), `src/api/common.c` or where `odin3_version_string` lives; Create `tools/release/make-tarball`; Test `tests/tools/test_release.py`.

- [ ] Step 1: tests: `test_version_is_1_0_0` (`odin3 --version` and `odin3_version_string` print `1.0.0`); `test_install_tree` (`cmake --install` into a temp prefix: `bin/odin3`, `lib/libodin3.so`, `include/odin3/odin3.h`, `share/odin3/lib/*.o3lib`, `share/odin3/tools/flow/odin3_flow.py`); `test_tarball_has_submodules` (tarball lists `third_party/abc/` and `third_party/unity/` sources and a `SHA256SUMS`).
- [ ] Step 2: run, expect failures.
- [ ] Step 3: implement.
- [ ] Step 4: tests pass; lint.
- [ ] Step 5: commit and push.

### Task 11: pip package

**Files:** Create `pyproject.toml` build section for scikit-build-core (package `odin3`), `python/odin3/__init__.py` (re-exports `plugins/python/odin3.py`; the module moves or is copied at build, decide by keeping one source file), Modify `plugins/python/odin3.py` (`find_library()`: bundled first, then `$ODIN3_LIB`, then build tree; returns the path and logs which); Test `tests/tools/test_wheel.py` (sweep label: builds the wheel in a temp venv).

- [ ] Step 1: tests: `wheel_prefers_bundled_lib` (Review Focus 5); `test_wheel_installs_and_reads` (temp venv, `pip install` the built wheel, `python -c` reads a micro BLIF and prints the module name).
- [ ] Step 2: run, expect failures.
- [ ] Step 3: implement; run `auditwheel show`/`repair` when available, else keep `linux_x86_64` and record it for the release notes.
- [ ] Step 4: tests pass (sweep, heavy-run); lint.
- [ ] Step 5: commit and push.

### Task 12: CHANGELOG, release notes, clean-machine test

**Files:** Create `CHANGELOG.md`, `docs/RELEASE-NOTES-1.0.md`, `tools/release/clean-check` (script), CTest `release_clean` (sweep); Modify `docs/PHASE7.md` (ticks, numbers), `docs/log_cost_of_tokens_and_time.md` (controller).

- [ ] Step 1: `release_clean`: fresh clone of the branch into an empty temp dir with a minimal env (`env -i` plus PATH), build Release, install, `pip install` the wheel, run the getting-started commands of `docs/user/getting-started.md` (extracted from its code blocks): exit 0.
- [ ] Step 2: run, expect failure until the pieces exist; fix what it finds.
- [ ] Step 3: write CHANGELOG (from `docs/PHASE*.md` checklists and PR titles) and release notes (wheel tag, known gaps, D12).
- [ ] Step 4: `release_clean` passes; docs build; lint.
- [ ] Step 5: commit and push. The tag `v1.0.0` and the GitHub release: Peter, or the controller after his explicit go (spec P-7).
