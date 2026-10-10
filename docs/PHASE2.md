# Odin III — Phase 2: Verilog-2005 front end

Spec: `docs/DESIGN.md` §4 (§4.0 project input, §4.1 Verilog-2005), §6 steps 1–4, §12. Exit test
(decision #4): every Verilog micro in `tests/micro/verilog` and every VTR benchmark in the Phase 2
test set reads, elaborates and runs `proc`/`opt`, and the result is identical (`netlist-compare`)
or equivalent (`equiv-check`) to the Parmys golden; micros Parmys itself rejects are listed
exceptions. The phase-2 project-corpus cases (`tests/golden/projects`) read in all their Phase 2
formats and match their oracles.

Each sub-project gets its own spec (`docs/specs/`), plan, and PRs. Model/effort per
`docs/PHASE0.md` §9 (grammar, preprocessor, readers: Opus `high`; elaboration and `proc`
semantics: Opus `xhigh` → `high`); the agent says when to switch.

## Sub-projects (decision #1)

| # | Sub-project | Depends on | Review | Model/effort |
|---|---|---|---|---|
| 2A | Source manager, AST, symbol table (shaped for the Phase 5 slang adapter) | Phase 1 | Peter (AST is a design rule) | Opus `xhigh` |
| 2B | Project readers in C (`.o3proj`, `-f`, `.qsf`/`.qpf`/`.qip`) against the corpus; preprocessor | 2A | light | Opus `high` |
| 2C | Verilog-2005 parser (Bison/Flex, Odin II lineage) covering the synthesizable subset (#3) | 2A | light | Opus `high` |
| 2D | Elaboration: parameters, `generate`, functions/tasks, signedness → word-level IR with processes | 2C | Peter (semantics) | Opus `xhigh` |
| 2E | `proc`: `always` → mux trees + `$dff*`, latch detection | 2D | light | Opus `xhigh` → `high` |
| 2F | `opt`: constant folding, dead-node removal, CSE, mux collapsing | 2D | light | Opus `high` |
| 2H | Exit-test harness: micros + VTR benchmarks vs Parmys, smallest first; fuzzing vs Yosys starts | grows alongside | light | Opus `high`; Sonnet for bulk |

Order: 2A → 2B ‖ 2C → 2D → 2E ‖ 2F, with 2H growing alongside.

## Checklist

- [ ] 2A AST + symbol table spec approved, implemented, merged
- [ ] 2B project readers pass the corpus (Phase 2 formats); preprocessor
- [ ] 2C parser covers the synthesizable Verilog-2005 subset (coverage table, #3)
- [ ] 2D elaboration to word-level IR
- [ ] 2E `proc`
- [ ] 2F `opt`
- [ ] 2H micros + VTR benchmark set identical/equivalent to Parmys

## Decisions log

1. Phase 2 split as above, order 2A → 2B ‖ 2C → 2D → 2E ‖ 2F, 2H alongside (Peter, 2026-10-09).
2. **Titan deferred** (Peter, 2026-10-09): the test space for Phase 2 is the VTR benchmarks
   (`vtr_flow/benchmarks/verilog`, the larger designs included: `mcml`, `arm_core`, `LU*PEEng`,
   `bgm`, `stereovision*`, `mkDelayWorker32B`, …) plus the micros; the Titan download and the
   "smallest Titan design parsed from its `.qsf`" deliverable move to Phase 6 with VQM. Primitive
   library v0 (Altera cells for Titan) moves with it (agent default; it has no Phase 2 consumer).
3. **Parser**: Bison/Flex in the Odin II lineage (Peter, 2026-10-09). Before 2C, a coverage audit
   lists every synthesizable Verilog-2005 construct (IEEE 1364-2005, the IEEE 1364.1 synthesis
   subset, and the textbooks in `~/odin3-ws/BOOK_VERILOG_VHDL/`: Chu, *FPGA Prototyping by Verilog
   Examples*; Bhasker, *Verilog HDL Synthesis*) against the Odin II grammar, the micros and the VTR
   benchmarks; gaps become 2C requirements (`docs/specs/2026-10-09-2C-verilog-coverage.md`).
4. Exit bar: identical under `netlist-compare`, else equivalent under `equiv-check`, against Parmys;
   Odin II goldens are read-only references; Parmys-rejected micros are listed exceptions (Peter,
   2026-10-09).
5. Phase 1 agent defaults reviewed and kept (Peter, 2026-10-09): black-box ports matched by name
   (IR-7b), the simulator scratch-sizing hook, Yosys-simlib word-cell semantics and `$pmux` OR, the
   2^20 reader width cap, `odin3_attr_foreach` and `odin3_design_set_top`, ABI v3 naming and the
   enum move into `odin3.h`, the 1C BLIF rules, the project-grammar decisions of PHASE1 #19, and
   1G's decisions numbered #20/#21.
6. **Coverage audit** (`docs/specs/2026-10-09-2C-verilog-coverage.md`, 159 constructs: 100 MUST, 25
   SHOULD, 10 IGNORE, 24 REJECT) is the requirement list for 2C/2D; Odin II's grammar is a starting
   point, not a spec. Its decisions (Peter, 2026-10-09):
   - D1 `full_case`/`parallel_case` (attributes and `// synopsys` metacomments) honoured as Yosys
     does, with a warning per use;
   - D2 initial values: constant `initial` assignments, declaration initialisers and
     `$readmemh/b` set FF/RAM init values; other `initial` content ignored with a warning;
   - D3 tri-state: parse all; `z` drives elaborate to `$tribuf` only on top-level `inout`/output
     ports; an internal tri-state net is a located error until the IR has a tri-state net model;
   - D4 `wand/wor/triand/trior` resolve as AND/OR of their drivers; `uwire` is a `wire` with a
     single-driver check;
   - D5 illegal-but-Parmys-accepted forms (`input reg`, `input integer`) accepted with a located
     warning.
7. **AST (2A)** (Peter, 2026-10-09): arena storage with typed 32-bit IDs like the IR (compact node
   records: kind, location, child span, per-kind payload; explicit-stack traversal); one AST for
   parsed and elaborated forms (the slang adapter emits the elaborated subset); locations are
   file/line/col plus the macro-expansion and include chain; the symbol table is built during
   elaboration (2D), the parser records names only. Agent defaults: comments in a side table keyed
   by location; `(* *)` attributes kept on AST nodes and carried to the IR; the AST is freed after
   elaboration unless a pass asks to keep it.
8. **2A spec approved** (`docs/specs/2026-10-09-2A-ast-design.md`; agent default under Peter's
   overnight rule, 2026-10-10, after three Opus review rounds): Clang-style location space with
   EXPANSION / MACRO_ARG (one per spelled run) / SCRATCH buffers and expansion ranges; a 24-byte
   node record, 68 kinds with a normative slot table and per-kind flags; builder-enforced depth cap
   `ODIN3_AST_MAX_DEPTH` = 32768 (measured deepest real chain: 2,032 terms in Koios `lenet.v`).
   Open questions decided as recommended: Q1 the design owns the source manager, per-run ASTs and
   symbol tables, and `odin3_srcloc` gains the raw `loc` (1B struct change); Q2 AST strings in the
   design strtab; Q3 2D in two stages with the elaborated AST as the seam (slang enters at stage 2);
   Q4 the depth cap above; Q5 a Verilog printer in 2A (parse–print–parse oracle for 2C).
   *Measured (2A Task 7, `build/release/bench_ast`, Release, dev machine, 2026-10-10, output
   verbatim):*

   ```
   mcml.v buffer                     : 650837 bytes (expected 650837), 24508 lines (expected 24507), line map + buffer table 131992 bytes
   mcml.v decode (random locs)       :     0.612 s      16.3 M/s  (target >= 10 M/s: ok)
   mcml.v segment cursor (sequential):     0.021 s     467.0 M/s  (target >= 10 M/s: ok)
   estimate (200k nodes, 20k comments): 200002 nodes, AST+child table+payloads+comments 9863168 B, srcman 131992 B, total 9995160 B (budget 20,000,000: ok) -- an estimate until 2C's --parse-only run
   build (with locs)                 :     0.100 s  2000004 nodes (target <= 1.000 s: ok)
   bytes per node (with locs)        :     47.63  AST reserved 95256576 B (target <= 40: MISSED); srcman 1049496 B
   breakdown (reserved/used B)       : nodes 48070656; child table 16777216/9777956 (2444489 entries); payload recs 12582912/6666600 (277775); arena 17825792
   bytes per node, used              :     41.17  (nodes + child entries + payload recs + arena reserved)
   mix                               : IDENT 36.1% BINARY 22.2% NUMBER 13.9% SELECT 11.1% other 16.7%
   walk                              :     0.015 s  2000004 nodes (target <= 0.100 s: ok)
   cursor                            :     0.022 s  4000008 events
   check FULL (with locations)       :     0.126 s  status 0
   check FAST (with locations)       :     0.041 s  status 0
   check FULL target <= 0.300 s       : ok
   destroy                           :     0.002 s
   build (no locs)                   :     0.087 s  2000004 nodes (target <= 1.000 s: ok)
   bytes per node (no locs)          :     47.63  AST reserved 95256576 B (target <= 40: MISSED); srcman 920 B
   breakdown (reserved/used B)       : nodes 48070656; child table 16777216/9777956 (2444489 entries); payload recs 12582912/6666600 (277775); arena 17825792
   bytes per node, used              :     41.17  (nodes + child entries + payload recs + arena reserved)
   check FULL (no locations)         :     0.064 s  status 0
   rule-1 location decode cost       :     0.061 s  (FULL with locs - FULL without)
   max RSS                           : 94308 KiB
   ```

   Targets met: decode and segment cursor >= 10 M/s; the 200k-node mcml estimate (about 10.0 MB
   against 20 MB; an estimate until 2C's `--parse-only` run); build, walk and check FULL (0.125 s
   with locations, against 0.300 s). The rule-1 location-decode cost inside check FULL is 0.063 s
   of the 0.125 s (FULL with locations minus FULL on the same tree without locations). The mcml.v
   buffer reports 24,508 lines: 24,507 newlines (the file ends in one) plus the implicit line 1.

   **Missed: bytes per node.** 47.63 reserved (41.17 counting used vec entries but reserved
   arena) against <= 40. Not relaxed. The 24-byte record is 48.1 MB (24 B/node) as designed; the
   rest is the child table (2.44M entries, 16.8 MB reserved, doubling slack), payload records
   (278k NUMBERs x 24 B, 12.6 MB reserved) and the arena (17.8 MB, 64 B per NUMBER: a 24-byte
   `odin3_ast_number` plus 32 one-byte bits for an unsized decimal, rounded to 16). A NUMBER
   therefore costs about 88 B; at 14% of nodes that is about 12 B/node. Candidates for a later
   round (Peter's call, each a spec change): pack unsized literals up to 64 bits inline in the
   payload record, share identical literals' payloads, grow the child table and payload vecs by
   1.5x or trim at `finish`. The 200k mcml estimate already fits its budget with 2x headroom.
9. **2B spec approved** (`docs/specs/2026-10-10-2B-project-preproc-design.md`; agent default under
   Peter's overnight rule, 2026-10-10, after an Opus review and two re-reviews): project record owned
   by the design, one C reader per format byte-identical to the corpus oracle (oracle wins where they
   differed: `-f` inside `-F` resolves against the outermost list; oracle amended to read with
   `newline=""` and to fix its Unicode-digit crash); a whole-file substitute-then-rescan
   preprocessor emitting the 2A segment map (one EXPANSION per use, one MACRO_ARG per spelled run,
   loc-0 bytes as their own segments, actuals collected across frames, recursion judged on the
   spelling ancestry, a per-buffer chain-length cap); caps against include/expansion/location
   bombs; corpus CTest, pp-diff against Icarus/Yosys where they agree, model-based and fuzz tests.
   Open points as recommended: `--top` replaces the project top (info line); SV macro operators
   are a located error until Phase 5; macro argument defaults and empty actuals accepted; an
   undefined macro is an error; the Odin II XML reader lands in 2B (DESIGN §4.0 "after Phase 2"
   becomes "with 2B"); the oracle is tightened where the corpus is silent. Declared divergences
   (directives inside macro bodies, the w1 self-application case) are listed in the spec §10.4.
