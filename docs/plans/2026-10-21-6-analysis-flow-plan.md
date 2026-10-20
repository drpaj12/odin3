# Phase 6 analysis flow and provenance (6.1 to 6.5): Implementation Plan (v1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Steps use checkbox
> (`- [ ]`) syntax. Commit and push after every task and after each rule or sub-step inside a long
> task. Never use `cd X && git ...`; use `git -C` and absolute paths. Tests first.

v1 (2026-10-21). Spec: `docs/specs/2026-10-21-6-analysis-flow-design.md` v1 (rule IDs G, C, E, V, P are
binding). **Peter approves the spec before Task 1 of any PR starts** (CLAUDE.md rule 2). Its nine
questions are taken as agent defaults (logged by Task 12 as a PHASE6 decision). **Stop and report** if a
task needs an IR field, ID, invariant or check-rule change, an ABI change, or a change to how an existing
reader or writer behaves without its new option.

**Goal:** the research flow works end to end on small designs: a gate-level netlist is read, drawn,
levelized, exported to Python graph tools, and the coverage of provenance to HDL is measured and gated.

**PR split (one pass or tool per PR, CLAUDE.md rule 1):**

| PR | Tasks | Content | Depends on |
|---|---|---|---|
| 6.4 | 1 | `levelize` pass | nothing |
| 6.2 | 2 | `write_dot --color-by`, `--locs` | nothing for colours; `--locs` needs `odin3_hdl_locs` (5.4a, merged) |
| 6.1 | 3, 4 | netlist fixtures and the conformance tool; fixes if defects | nothing; the `src` fixture leg needs 5.4c |
| 6.3 | 5, 6 | `write_json --odin3`, `read_json` handlers, `odin3_graph.py` | 5.3 (`read_json`, merged), 6.4 for the level key (tests use a hand-set attribute until 6.4 merges) |
| 6.5a | 7 | `dump_locs --types` | 5.4a |
| 6.5b | 8, 9, 10 | `tools/prov-cover`, legs (a) to (c), CTests, small sweep | 6.5a, 5.4b, 5.4c, 5.4d, 5.1 |
| 6.5c | 11 | leg (d), VPR placed blocks (heavy) | 6.5b |
| docs | 12 | PHASE6.md numbers, decisions, PASSES.md, DESIGN 9 | all |

**Parallel lanes (at most three).** Wave 1, all disjoint files, start at once: **6.4** (`src/passes/
levelize/`), **6.2** (`src/backends/dot/`, `write_formats.c`), **6.1** (`tests/netlist/`,
`tools/netlist-ingest/`). Wave 2: **6.3** after 6.4 merges (it uses the level key; its handler work can
start earlier against a hand-set attribute) and **6.5a** (touches `dump_locs.c` only). Wave 3: **6.5b**
once 5.4b, 5.4c, 5.4d have merged (read-back legs), then **6.5c**. 6.2 and 6.3 both edit
`src/passes/write_formats.c` (option parsing): the second to merge merges `origin/main` and notes it
in Deviations. 6.1, 6.4, 6.2 never depend on 5.4 except as marked; 6.5 b and c are the only items that
wait on it.

**Branches:** `feat/6.4-levelize`, `feat/6.2-dot-color`, `feat/6.1-netlist-ingest`, `feat/6.3-graph-export`,
`feat/6.5a-dump-types`, `feat/6.5b-prov-cover`, `feat/6.5c-prov-cover-vpr` (each from `main`; later ones
`git merge origin/main`, merge only). No PR from the lane; the controller reviews.

**Build and test:** `cmake --preset debug`; `/home/jamiespa/odin3-ws/work/controller-scripts/build-mem.sh
--preset debug`; `env -u ODIN3_HEAVY_OK ctest --preset debug -R <targeted> -j6`; full debug ctest and
`tools/lint.sh` before the last push of each PR. Only Task 11 runs under `tools/heavy-run`.

## Task 1 (6.4): `levelize` (V-1 to V-8)

**Files:** new `src/passes/levelize/levelize.c` (and a header if a unit test needs one), new
`tests/unit/test_levelize.c`, `src/passes/builtin.{c,h}` (register last), `CMakeLists.txt`,
`docs/PASSES.md`.
**Produces:** pass `levelize [--key NAME]`; attributes `odin3_level` (cells, nets) and `<key>_cut`.
**Tests:** `test_levelize` (hand counts of V-7 a, loop cases V-7 b with the warning text and
location, rerun identity, `$proc` refusal, 100000-cell chain), `levelize_micro_<name>` script tests on
three micros that dump the attribute with `write_json` (the plain attribute path, no 6.3 key yet).
- [ ] Step 1: write the tests; run: fail (pass unknown).
- [ ] Step 2: Kahn sweep over cells in ID order using the registry's sequential flags
  (`odin3_sim_is_seq`-equivalent public flag; read `src/ir/celltype.h` and `src/passes/abc/region.c`
  for the exact accessor and copy the loop-cut rule, do not reuse the abc types).
- [ ] Step 3: green, lint, commit and push.

**Acceptance:** V-7 a to e pass; no recursion (V-6); attribute-only mutation (a structural dump
before and after is equal); registry-order test updated with the new row only.

## Task 2 (6.2): `write_dot --color-by` and `--locs` (C-1 to C-8)

**Files:** `src/backends/dot/writer.{h,c}` (opts: `color_attr`, `locs`; legend cluster, palette),
`src/passes/write_formats.c` (option parsing for `write_dot` only), `tests/unit/test_dot_writer.c` (or
the existing dot test, found with the graph), `tests/` dot script tests, `docs/PASSES.md`.
**Produces:** the two options; a legend cluster; the second label line.
**Tests:** C-8 a to f. Existing dot tests unchanged (no-option output byte-identical).
- [ ] Step 1: tests for the colour classes, legend counts and `(none)`; the 13-value warning; missing
  attribute warning; `@type`; `--locs` with Verilog and BLIF-read designs; the `nop` check when
  Graphviz is installed (record in the lane report whether it ran).
- [ ] Step 2: implement in small functions (the file has a complexity gate; split palette, legend
  and label code out of `dot_emit_node`).
- [ ] Step 3: green, lint, commit and push.

**Acceptance:** C-1 to C-8; `--help` text and PASSES.md row updated; budget and focus tests unchanged.

## Task 3 (6.1): netlist fixtures (G-2, G-4, G-6, G-7)

**Files:** new `tests/netlist/cells/cmos.v`, `tests/netlist/<micro>/{rtl.v, mapped.v, mapped.blif,
mapped_src.v, gen.sh}`, `tests/netlist/README.md` (how generated, Yosys 0.55 command lines).
**Produces:** committed fixtures for 6 to 8 micros (list fixed here: the `tests/micro/verilog` entries
chosen by the lane and named in its report, plus `regen` and `cone`).
**Tests:** `netlist_cells_elab` (each model elaborates alone and as `--lib`).
- [ ] Step 1: write the cell models and the two hand micros; elaborate them.
- [ ] Step 2: write `gen.sh` (Yosys maps to exactly the G-2 set; one run with `src` kept, one with
  `-noattr` for the reverse-engineering case) and run it once; commit the outputs.
- [ ] Step 3: commit and push.

**Acceptance:** every fixture reads with `read_verilog` (library via `-v`) or `read_blif`; sizes small
(each file under 20 KB).

## Task 4 (6.1): conformance tool and defect fixes (G-1, G-3, G-5, G-6)

**Files:** new `tools/netlist-ingest/` (shell wrapper plus Python, same style as `tools/equiv-check`),
CTest entries `netlist_ingest_<micro>_{verilog,blif}` and `netlist_dump_<micro>`; fixes (own commits,
reported) under `src/frontends/` if found.
**Tests:** G-5 equivalence by `tools/equiv-check`; G-4 name check against the fixture; G-6 record kinds and
the `src` restoration (skipped until 5.4c is merged, with a named skip); G-7 unknown cell error and
`--auto-blackbox`.
- [ ] Step 1: write the tool against the fixtures; run: record defects.
- [ ] Step 2: fix each defect narrowest-layer first; if a fix changes an existing golden, stop and report.
- [ ] Step 3: green, lint, commit and push.

**Acceptance:** all micros equivalent in both forms; hierarchy and names kept; zero unreported reader
changes.

## Task 5 (6.3): `write_json --odin3` and the reader handlers (E-1 to E-3, E-8)

**Files:** `src/backends/json/{emit.c,writer.c,writer.h}` (opts `odin3_keys`), `src/passes/
write_formats.c`, `src/frontends/json/reader_ext.c` (handlers in `odin3_json_ext_keys[]`),
`tests/unit/test_json_odin3.c`, `docs/PASSES.md`, `docs/DESIGN.md` 9 (JSON row).
**Tests:** without the option the output is byte-identical to a stored golden of today; with it, key
order and values for a micro (levels hand-set by attribute until 6.4); write, read, write is
byte-equal; `odin3_version` 2 is the J-9 error; `odin3_fsm_*` attributes of an `fsm` micro appear in
`attributes`.
- [ ] Step 1: tests; Step 2: writer; Step 3: handlers; Step 4: green, lint, commit and push.

**Acceptance:** E-1, E-2, E-3, E-8; `json-check`/5.3 round-trip tests untouched and green.

## Task 6 (6.3): Python loader (E-4 to E-7)

**Files:** new `plugins/python/odin3_graph.py`, new test `tests/python/` (or the existing Python test
location, found by the lane) plus CTest `graph_export_counts`, `graph_loader_isolated`.
**Tests:** E-6 counts against `walk.py`; E-5 isolated run (`python3 -I` of a temp copy, no `libodin3`);
E-7 level ordering after `levelize` (skipped with a named skip until 6.4 merges); `to_pyg` when
torch_geometric imports, else a named skip.
- [ ] Step 1: write the E-4 counting rule in both the loader and the test helper and the tests; Step 2:
  implement `load_json` and `to_pyg`; Step 3: green, lint (ruff, mypy), commit and push.

**Acceptance:** counts equal on every micro of the default set; no skip is silent.

## Task 7 (6.5a): `dump_locs --types`

**Files:** `src/passes/dump_locs.c`, its unit test, `docs/PASSES.md`.
**Produces:** with `--types` each node line is `module|node|name|TYPE|SRC`, escaped like the other
fields; without the option the output is byte-identical.
**Tests:** unchanged-output test, typed output test with `$lut`, `$_DFF_P_`, `lpm_add_sub`.
**Acceptance:** `tools/loc-check` tests untouched and green.

## Task 8 (6.5b): the coverage tool core (P-1, P-3, P-5, P-8)

**Files:** new `tools/prov-cover/{prov-cover,prov_cover.py,test_prov_cover.py}`.
**Produces:** parser of typed dumps; class assignment (`lut`, `latch`, `hard`, `lpm`, and the
exceptions `const`, `writer-buffer`, `inherited`, `pad`); coverage, median, p90, `capped`; report
JSON and table; exit status per P-4.
**Tests:** P-8 unit tests (pure Python, no odin3 binary).
- [ ] Steps: tests first, implement, `ruff`/`mypy`, commit and push.

**Acceptance:** an unresolved plain LUT fails the gate; each exception class is counted and printed.

## Task 9 (6.5b): legs (a) to (c) and CTests (P-2, P-4, P-7)

**Files:** `tools/prov-cover/legs.py`, CTest `prov_cover_<micro>` over the default micro set,
`prov_cover_sweep` (needs `ODIN3_HEAVY_OK`, not default).
**Tests:** micros at LUT level through `abc` (leg a), written and read back (b), LPM Verilog (c).
- [ ] Step 1: wire the flow commands (`read_verilog; ...; abc; write_blif --vtr --name-style provenance;
  dump_locs --types` and the read-back with `read_blif --hdl-locs`; the LPM legs with the 5.1 passes
  and `write_verilog` flavour, read with `read_verilog -netlist`). Take exact options from
  `docs/PASSES.md` at the time; do not guess flags.
- [ ] Step 2: run the micros; classify every unresolved object; any outside P-3 is a defect to report
  (fix in the right earlier pass only with Peter's attention; otherwise list in Deviations).
- [ ] Step 3: commit and push.

**Acceptance:** the default micro set passes the gate on legs a to c, or each failure is a reported
defect with a one-line reproduction.

## Task 10 (6.5b): the Phase 3 sweep, small first

**Files:** none new; run `prov_cover_sweep` on the Phase 3 set for legs (a) and (b) and the
Phase 5 LPM micros for (c), smallest input first, under `tools/heavy-run` only if the lanes are idle.
**Output:** `report.json` per design in the lane's scratch, summary table pasted into the PR report.
**Acceptance:** coverage numbers recorded per design; failures classified.

## Task 11 (6.5c): VPR placed blocks (P-4 leg d)

**Files:** `tools/prov-cover/vpr_leg.py`, a `.place` fixture and a netlist, unit tests.
- [ ] Step 1: take one small design through `tools/vtr-qor` (read how it keeps run directories; read
  VPR's `.place` format from `$VTR_ROOT` docs, do not guess) and write the block-name to netlist-name
  rule down in the tool's docstring with the evidence.
- [ ] Step 2: tests on the fixture (a LUT block, a latch block, a hard block, a pad, an unresolved
  block), then the tool.
- [ ] Step 3: run on the Phase 3 set, smallest first, under `tools/heavy-run` (lanes idle).

**Acceptance:** every placed block resolves or is a `pad`; numbers recorded; the rule's evidence
cited. If the mapping cannot be made exact, stop and report with the cases found.

## Task 12 (docs)

**Files:** `docs/PHASE6.md` (tick items, numbers table per design and leg, decisions log entries for the
nine questions as agent defaults), `docs/PASSES.md`, `docs/DESIGN.md` 9 (JSON row), `docs/
log_cost_of_tokens_and_time.md` by the controller. **Acceptance:** `tools/lint.sh` green.

## Review focus (for the critique agent, one pass per PR)

- 6.4: the loop-cut rule matches abc's (lowest ID, one warning each); no recursion; sequential
  flags read from the registry, not a type-name list; attribute-only mutation.
- 6.2: no change without the options (byte-identical); palette determinism; legend outside the budget;
  escaping of attribute values (quotes, `&`); label complexity gate.
- 6.1: no reader behaviour changed silently; fixtures small and regenerable; the `src`/no-`src`
  distinction is real (locations printed only for `SOURCE` leaves).
- 6.3: default JSON bytes unchanged; key order fixed; the loader imports nothing from the repo; count
  rule identical on both sides; version policy.
- 6.5: exception classes are narrow and counted (no catch-all); a netlist location never counts as
  an HDL location; `inherited` reported both ways; no flag guessed in the tool.
- All: one pass or tool per PR; no IR, ABI or ADR change; docs for each option.

## Risks

1. Gate-level Verilog may expose elaboration gaps (Task 4); fixes are separate commits and reported.
2. 6.2 and 6.3 both edit `write_formats.c`; a merge conflict is expected and trivial.
3. networkx or Torch absent: named skips, never silent passes.
4. The 8-location cap lowers specificity numbers on merged logic (P-5 notes it).
5. VPR block naming unverified (Task 11 starts by verifying).
6. Heavy lock: Tasks 10 (large designs) and 11 stall other lanes; run only when idle.
7. 5.4b, 5.4c, 5.4d late: 6.5b legs (b) and (c) are the only blocked part; legs (a) and the tool core go
   ahead.

## Deviations

(empty; each lane appends what it changed from this plan and why)
