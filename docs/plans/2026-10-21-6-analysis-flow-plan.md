# Phase 6 analysis flow and provenance (6.1 to 6.5): Implementation Plan (v1.1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Steps use checkbox
> (`- [ ]`) syntax. Commit and push after every task and after each rule or sub-step inside a long
> task. Never use `cd X && git ...`; use `git -C` and absolute paths. Tests first.

v1.1 (2026-10-21; v1.1: critique fixes, tracks spec v1.1). Spec: `docs/specs/2026-10-21-6-analysis-flow-design.md`
v1.1 (rule IDs G, C, E, V, P are binding). **Peter approves the spec before Task 1 of any PR starts** (CLAUDE.md rule 2). Its eleven
questions are taken as agent defaults (logged by Task 12 as a PHASE6 decision), except that Questions
5 and 7 (the `inherited` and `pad` exception classes, not allowed by PHASE6 decision 2) stay off in the
tool until Peter approves them. **Stop and report** if a
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

**Parallel lanes (at most three).** Wave 1 starts at once; the source directories are disjoint, but
the lanes share `docs/PASSES.md` and `CMakeLists.txt` (and 6.2 and 6.3 `write_formats.c`): each lane
merges `origin/main` before its PR, and the controller resolves those files (rows and test
registrations are additive) at merge: **6.4** (`src/passes/
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
**Tests:** `test_levelize` (hand counts of V-7 a, loop cases V-7 b with the warning text, the named
cell on the loop and its location, rerun identity, `$proc` and word-view `$dff` refusal, `--key`
guard, `$_DFF_P_`/`$_DFF_N_`/`$_DLATCH_P_` sources, the Q4 cases V-7 h to j (a RAM between two LUT chains
resets levels at its outputs and ends a path at its inputs; a black box with no stated latch passes
levels through; one with a stated latch or register follows the memory rule: inputs end paths, outputs are level 0), the V-6 100000-cell chain with its 10 s and 256 MB
bounds), `levelize_micro_<name>` script tests on
three micros that dump the attribute with `write_json` (the plain attribute path, no 6.3 key yet).
- [ ] Step 1: write the tests; run: fail (pass unknown).
- [ ] Step 2: Kahn sweep over cells in ID order using the registry's sequential flags
  (`odin3_sim_is_seq`-equivalent public flag; read `src/ir/celltype.h` and `src/passes/abc/region.c`
  for the exact accessor and copy the loop-cut rule, do not reuse the abc types). Only the
  bit-level storage types carry the flags (word `$dff` and the like do not): refuse word view (V-1).
  On a stall pick the lowest-ID cell that lies on a cycle of the remaining set (V-4).
  Memory cells: inputs are endpoints, outputs level-0 sources; black boxes that state a latch or register follow
  the same rule, all others are combinational (V-2). Level counting only: no delays or timing arcs.
- [ ] Step 3: green, lint, commit and push.

**Acceptance:** V-7 a to e and g to j pass; no recursion (V-6); attribute-only mutation (a structural dump
before and after is equal); `--key` guard (V-1); registry-order test updated with the new row only.

## Task 2 (6.2): `write_dot --color-by` and `--locs` (C-1 to C-8)

**Files:** `src/backends/dot/writer.{h,c}` (opts: `color_attr`, `locs`; legend cluster, palette),
`src/passes/write_formats.c` (option parsing for `write_dot` only), `tests/unit/test_dot_writer.c` (or
the existing dot test, found with the graph), `tests/` dot script tests, `docs/PASSES.md`.
**Produces:** the two options; a legend cluster; the second label line.
**Tests:** C-8 a to f (palette of the spec's twelve hex values, legend last). Existing dot tests
unchanged (no-option output byte-identical). When `dot` is absent the `nop` test prints the named
skip `SKIP graphviz_nop: dot not installed`.
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
**Produces:** committed fixtures for 9 micros (the G-7 list, fixed here): `tests/micro/verilog/`
`common/{dffsr,dff,dffe,adff,mux,adder}.v` and `micro/bm_DL_Dff_w_synch_reset.v`, plus the hand-written
`regen` and `cone`. `dffsr` is the one that exercises the `DFFSR` cell (`always @(posedge rst, posedge
clk)`). Also `tests/netlist/cells/cmos_map.v` (the Yosys techmap file).
**Tests:** `netlist_cells_elab` (each model elaborates alone and as `--lib`).
- [ ] Step 1: write the cell models and the two hand micros; elaborate them.
- [ ] Step 2: write `gen.sh` (Yosys 0.55 maps to exactly the G-2 set: `synth -flatten`, `dfflegalize`
  to `$_DFF_P_` and `$_DFFSR_PPP_`, `abc -g NAND,NOR`, `techmap -map cells/cmos_map.v`, `check -assert`
  so any other cell fails the script; verify the Yosys cell names first; one run with `src` kept, one
  with `-noattr` for the reverse-engineering case) and run it once; commit the outputs.
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
order and values for a micro (levels hand-set by attribute until 6.4); write A, read, write B, read, write C: A equals B after
the `odin3_prov` objects are dropped, and B equals C byte-for-byte (E-3; no provenance-record change,
a failure of B equals C is reported, Question 10); under `--odin3` the level is the key only, not
repeated in `attributes` (E-2); `odin3_version` 2 is the J-9 error; `odin3_fsm_*` attributes of an `fsm` micro appear in
`attributes`.
- [ ] Step 1: tests; Step 2: writer; Step 3: handlers; Step 4: green, lint, commit and push.

**Acceptance:** E-1, E-2, E-3, E-8; `json-check`/5.3 round-trip tests untouched and green.

## Task 6 (6.3): Python loader (E-4 to E-7)

**Files:** new `plugins/python/odin3_graph.py`, new tests in `tests/tools/` (the Python tests live
there; there is no `tests/python/`) plus CTest `graph_export_counts`, `graph_loader_isolated`.
**Tests:** E-6 counts against the IR by `dump_locs` and the C ABI (`walk.py`), not by any code the
loader shares (E-6); E-5 isolated run (`python3 -I` of a temp copy, no `libodin3`);
E-7 level ordering after `levelize` (strictly lower into combinational non-cut cells; edges into
sequential and cut cells exempt; skipped with a named skip until 6.4 merges); `to_pyg` when
torch_geometric imports, else a named skip.
- [ ] Step 1: write the E-4 counting rule in the loader; write the reference count in the test helper
  independently from the IR (`dump_locs` nodes, C-ABI pins), one node per port, one edge per bit
  pair; then the tests; Step 2:
  implement `load_json` and `to_pyg`; Step 3: green, lint (ruff, mypy), commit and push.

**Acceptance:** counts equal on every micro of the default set; no skip is silent.

## Task 7 (6.5a): `dump_locs --types`

**Files:** `src/passes/dump_locs.c`, its unit test, `docs/PASSES.md`.
**Produces:** with `--types` each node line is `module|node:TYPE|name|SRC` (the type inside the kind
field, escaped like the other fields; only the first `:` of the kind field splits; no extra field,
so the three-field parse rule at the top of `dump_locs.c` still holds, whose comment gets one
sentence); module, wire and net lines unchanged; without the option the output is byte-identical.
**Tests:** unchanged-output test, typed output test with `$lut`, `$_DFF_P_`, `lpm_add_sub`, a type
name containing `:` and `|`, and a reader check that tells the two formats apart by the kind field.
**Acceptance:** `tools/loc-check` tests untouched and green.

## Task 8 (6.5b): the coverage tool core (P-1, P-3, P-5, P-8)

**Files:** new `tools/prov-cover/{prov-cover,prov_cover.py,test_prov_cover.py}`.
**Produces:** parser of typed dumps (`node:TYPE`); class assignment (`lut`, `latch`, `hard`, `lpm`, and
the exceptions `const-driver`, `const-table`, `port-buffer`, `not-helper`, with the exact P-3 rules;
`inherited` and `pad` only with `--allow`, off by default); the written-file cross-check count per
class; coverage, median, p90, `capped`; report JSON and table with per-class counts for every class
(zero included), shares and the strict figure; exit status per P-4.
**Tests:** P-8 unit tests (pure Python, no odin3 binary).
- [ ] Steps: tests first, implement, `ruff`/`mypy`, commit and push.

**Acceptance:** an unresolved plain LUT fails the gate; each exception class is counted and printed
(every class, zero included) and equals its written-file count; a port-buffer whose net has a located
leg-(a) driver is a lost location, not an exception.

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
eleven questions as agent defaults; Questions 5 and 7 stay off until Peter approves), `docs/PASSES.md`, `docs/DESIGN.md` 9 (JSON row), `docs/
log_cost_of_tokens_and_time.md` by the controller. **Acceptance:** `tools/lint.sh` green.

## Review focus (for the critique agent, one pass per PR)

- 6.4: the loop-cut rule matches abc's (lowest ID, one warning each); no recursion; sequential
  flags read from the registry, not a type-name list; attribute-only mutation.
- 6.2: no change without the options (byte-identical); palette determinism; legend outside the budget;
  escaping of attribute values (quotes, `&`); label complexity gate.
- 6.1: no reader behaviour changed silently; fixtures small and regenerable; the `src`/no-`src`
  distinction is real (locations printed only for `SOURCE` leaves).
- 6.3: default JSON bytes unchanged; key order fixed; the loader imports nothing from the repo; count
  rule derived independently on each side (E-6); version policy.
- 6.5: exception classes are narrow and counted (no catch-all); a netlist location never counts as
  an HDL location; `inherited` reported both ways; no flag guessed in the tool.
- All: one pass or tool per PR; no IR, ABI or ADR change; docs for each option.

## Risks

1. Gate-level Verilog may expose elaboration gaps (Task 4); fixes are separate commits and reported.
2. Lanes share `docs/PASSES.md` and `CMakeLists.txt` (6.2 and 6.3 also `write_formats.c`); conflicts are
   expected and trivial: lanes merge `origin/main` before the PR and the controller resolves them.
3. networkx or Torch absent: named skips, never silent passes.
4. The 8-location cap lowers specificity numbers on merged logic (P-5 notes it).
5. VPR block naming unverified (Task 11 starts by verifying).
6. Heavy lock: Tasks 10 (large designs) and 11 stall other lanes; run only when idle.
7. 5.4b, 5.4c, 5.4d late: 6.5b legs (b) and (c) are the only blocked part; legs (a) and the tool core go
   ahead.
8. `levelize --key` collisions and `odin3_level` appearing in default JSON (spec risks 7, 8): the
   V-1 guard and the E-2 one-place rule are tested.
9. Exception classes weakening the gate (spec risk 9): per-class counts and the file cross-check are
   reviewed in 6.5b; `inherited` and `pad` stay off until Peter approves.

## Deviations

(each lane appends what it changed from this plan and why)

- 6.2 (write_dot): `odin3_dot_opts` gained `color_attr` and `locs`; every positional initializer of the
  struct (writer, pass, writer-check tool, existing dot tests) now lists the two new fields because
  `-Wmissing-field-initializers` is an error. No behaviour change there.
- 6.2: the legend is emitted after the edges, just before the closing brace (last in the file and after
  every module cluster). The `(none)` legend entry is omitted when no drawn node is in that class.
- 6.2: `+N` counts distinct locations of `odin3_hdl_locs`, which caps at 8 (X-3), so N is at most 7.
- 6.2: bits text is MSB first (`0`, `1`, `x`, `z`); COVER and PROC values give the class texts
  `(cover)` and `(proc)`.
- 6.2: the `nop` check uses `/usr/bin/nop -o /dev/null` (Graphviz 2.43 ran here); the skip line
  `SKIP graphviz_nop: dot not installed` is printed when `nop` is absent. The structural quote/brace check
  always runs. C-8 (b) "Verilog micro" is covered by a unit test with SOURCE provenance and by
  `cli_writers` (read_verilog, then `--locs`); a BLIF-read design is checked byte-identical with and
  without `--locs`.
- 6.2 fix round (agent default): the colour legend names at most 32 classes (`LEGEND_MAX`); the
  rest fold into one `(N more values)` entry with the neutral fill `#cfcfcf`. Node fills are
  unchanged. Dot text keeps valid UTF-8 and replaces DEL and invalid bytes with `?`. Class texts are
  interned once through a hash index (O(n)) and sorted (O(k log k)). `--color-by`, `--focus` and
  `--max-nodes` reject a value that starts with `--`. `test_graphviz_nop` finds `nop` on PATH and
  passes when it ran.

6.4 levelize (lane p6-levelize):

1. Ownership of an existing `<key>` (V-1). Attributes carry no author, so "written by levelize" cannot
   be told from a user INT of the same name. The pass accepts an existing `<key>` or `<key>_cut` that is
   an INT (a rerun, idempotence V-7 c) and refuses any other kind (`INVALID_ARG`, nothing written). A
   user INT under the default key is therefore overwritten. A `<key>_cut` left on a cell that is not cut
   in this run is removed. Fix round: with a custom `--key`, any existing `<key>` or `<key>_cut` of any
   kind is refused (`INVALID_ARG`), so a user value is never overwritten or deleted; the price is that a
   second run with the same custom key is refused too. The default key keeps the INT rerun rule.
2. V-7 (d), levels surviving `write_json --odin3` then `read_json`, is not testable on this branch (the
   option is lane 6.3). `test_levelize` and the `levelize_micro_*` script tests check the plain attribute
   in the default `write_json` text instead; (d) belongs to the integration check after 6.3 merges.
3. Library cells. The registry flags `SEQ_EDGE`/`SEQ_LEVEL` are not set on tech-library cells, so a
   library cell with a `seq` statement (generic_gates DFFP and the like) or a `memory` statement (hard
   RAM) is also a source (level 0 outputs, inputs end paths) through `odin3_celltype_lib`. A plugin
   black box with the registry flag follows the same rule. A cell that has both state and combinational
   outputs is treated as a whole: all its outputs are level 0.
4. INOUT pins count as drivers only (never as inputs), so a tristate or bus cell has no self-edge.
5. Loop cut cost. Each stall runs one iterative Tarjan over the unfinished cells, O(cells + pins) per
   stall. Fix round: one pass cuts the lowest-ID cell of every cyclic component (ascending ID order, one
   warning each), so K independent loops cost one pass; only loops that survive a cut (overlapping
   cycles in one component) need another.
6. Micros. The three `levelize_micro_*` tests start from committed Parmys BLIFs in `tests/golden/blif`
   (`ff`, `adder_hard_block`, `ansiportlist_2`; netlist view needs no front end) and
   `levelize_micro_word_refused` runs `pow`, whose `$pow` word cell must be refused.
7. Base. Branched from `origin/docs/phase6-spec` (PR #185 not merged when the lane started); merge
   `origin/main` after it merges.
8. Allocation failure (fix round). Attributes are written after the sweep of each module, so a
   `NO_MEMORY` can leave some attributes written. Cells, nets, pins and provenance never change; the
   pass help says so. `test_oom_leaves_structure_alone` injects a failure at every allocation point.
9. Memories without a library entry (fix round, agent default, Peter's rule: a memory's inputs are
   primary outputs, its outputs primary inputs). A black box named `single_port_ram` or
   `dual_port_ram` (BLIF `.subckt`, VTR primitives) or whose model is the `.subckt` of a
   `class="memory"` leaf of the loaded arch record is a source like a library `memory` cell.

6.3 graph export (lane p6-graph-export):

1. E-3 (i) as written (A equals B once `odin3_prov` is dropped) cannot hold for a file whose bit numbers
   have gaps: `read_json` numbers nets by ascending bit and the constants last, so the same A and B
   differ for a default `write_json` of ff.parmys (checked without `--odin3`). It is tested as what
   `--odin3` adds: A without the Odin III keys equals the default write of the design, B without them
   equals the default write after one read, and B equals C byte for byte (E-3 ii holds on every
   readable micro). `test_json_odin3` checks A equals B after dropping `odin3_prov` on a hand design
   without gaps.
2. Micro set (`graph_export_*`): 11 committed BLIFs of tests/golden/blif. dffsre.parmys,
   ansiportlist_2.parmys and hand_ports are counted (E-6) but left out of the round trip: `read_json`
   already refuses their default `write_json` output (check rule 4, a net with sinks and no driver),
   independent of `--odin3`; a test keeps them honest. pow.parmys (a `$pow`) is left out of the level
   tests because `levelize` refuses a word-level cell (V-1).
3. `odin3_level` is a JSON integer under `--odin3` and reads back as an INT attribute. The default
   attribute form is a 32-bit bit string and reads back as BITS, so "the same result as reading the
   attribute form" holds for the value, not the value kind.
4. `odin3_prov` is left out of an object that has no provenance record (no `kind` to state). Wires
   (multi-bit netnames) carry `odin3_level` only when the wire itself has the attribute (a net that
   belongs to a wire has no netname entry of its own); `levelize` writes it on nets, so those levels
   are not in the file. E-7 (cells) is unaffected.
5. `read_json` counts a valid `odin3_prov` in its own INFO line (`validated N odin3_prov objects (not
   restored)`). Fix round (agent default, since `--odin3` files are our own output): a malformed
   `odin3_prov` (not an object, kind not SOURCE/IMPORTED/DERIVED, hier not text, hdl not an array of at
   most 8 texts) or `odin3_level` (not an integer from 0 to INT32_MAX) is a located error, not a skipped
   member. `odin3_version` is still counted in the skipped `odin3_` count as before.
6. E-7: the C ABI has no getter for the registry flags (SEQ_EDGE, SEQ_LEVEL), and the lane may not
   change the ABI. The test exempts edges into a cell of level 0 (a register, latch or memory starts a
   path; a combinational cell with an input has level >= 1), into cut cells and into port nodes. No
   type-name list is used.
7. E-6 reference: nodes are the `dump_locs` node lines minus constant-driver cells (`$_CONST*_`, a
   `$sop` with no input pin), which the JSON does not write (E-4); edges are counted from the ABI
   over pins and nets, a net driven by a constant cell having none. Inout pins count as driver and
   sink.
8. The loader returns plain `parse_graph` lists too (nodes, edges) so the counts need no networkx; the
   networkx and Torch paths have their own CTest `graph_loader_packages` (a named skip without the
   packages, none installed here). pyproject.toml gains `ignore_missing_imports` for networkx, torch
   and torch_geometric (optional, imported lazily).
9. The Python CTests skip in an environment whose Python lacks cffi (`graph_export_counts`); they ran
   here with the repo venv's Python (cffi 2.1.1) and passed.
10. Fix round: `odin3_version` must be exactly the integer 1 in both readers (Python `type(v) is int`,
    so a bool is refused); 0, negative, above 1 and any other type are the J-9 error. An absent version
    is still accepted (a plain Yosys file).
11. Fix round: `odin3_level` is a key of cells and netnames only (E-2); a module with the attribute
    keeps it under attributes as the default writer does. The loader refuses a cell key equal to a port
    name with GraphError (node ids stay the plain key or name), and malformed JSON or structure raises
    GraphError (`main` prints it, exit 1). The HDL text of an object is built once, so a file the SRC
    writer skips warns once.
12. Fix round, E-7 non-vacuity: every micro with a cell must have a cell above level 0 and compare at
    least one edge. `hand_ports` has no cell at all (ports and black boxes only), so it is listed in
    `CELLLESS` and asserted to compare nothing.
