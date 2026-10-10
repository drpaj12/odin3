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
   verbatim, after `odin3_ast_finish` trims the child table, payload records, comments and units
   vecs to fit):*

   ```
   mcml.v buffer                     : 650837 bytes (expected 650837), 24508 lines (expected 24507), line map + buffer table 131992 bytes
   mcml.v decode (random locs)       :     0.611 s      16.4 M/s  (target >= 10 M/s: ok)
   mcml.v segment cursor (sequential):     0.021 s     475.3 M/s  (target >= 10 M/s: ok)
   estimate (200k nodes, 20k comments): 200002 nodes, AST+child table+payloads+comments 9366180 B, srcman 131992 B, total 9498172 B (budget 20,000,000: ok) -- an estimate until 2C's --parse-only run
   build (with locs)                 :     0.101 s  2000004 nodes (target <= 1.000 s: ok)
   bytes per node (with locs)        :     41.17  AST reserved 82341004 B (target <= 40: MISSED); srcman 1049496 B
   breakdown (reserved/used B)       : nodes 48070656; child table 9777956/9777956 (2444489 entries); payload recs 6666600/6666600 (277775); arena 17825792
   bytes per node, used              :     41.17  (nodes + child entries + payload recs + arena reserved)
   mix                               : IDENT 36.1% BINARY 22.2% NUMBER 13.9% SELECT 11.1% other 16.7%
   walk                              :     0.015 s  2000004 nodes (target <= 0.100 s: ok)
   cursor                            :     0.021 s  4000008 events
   check FULL (with locations)       :     0.127 s  status 0
   check FAST (with locations)       :     0.040 s  status 0
   check FULL target <= 0.300 s       : ok
   destroy                           :     0.002 s
   build (no locs)                   :     0.088 s  2000004 nodes (target <= 1.000 s: ok)
   bytes per node (no locs)          :     41.17  AST reserved 82341004 B (target <= 40: MISSED); srcman 920 B
   breakdown (reserved/used B)       : nodes 48070656; child table 9777956/9777956 (2444489 entries); payload recs 6666600/6666600 (277775); arena 17825792
   bytes per node, used              :     41.17  (nodes + child entries + payload recs + arena reserved)
   check FULL (no locations)         :     0.069 s  status 0
   rule-1 location decode cost       :     0.058 s  (FULL with locs - FULL without)
   max RSS                           : 94324 KiB
   ```

   Targets met: decode and segment cursor >= 10 M/s; the 200k-node mcml estimate (about 9.5 MB
   against 20 MB; an estimate until 2C's `--parse-only` run); build, walk and check FULL (0.125 s
   with locations, against 0.300 s). The rule-1 location-decode cost inside check FULL is about
   0.06 s (FULL with locations minus FULL on the same tree without locations). The mcml.v buffer
   reports 24,508 lines: 24,507 newlines (the file ends in one) plus the implicit line 1.

   **Bytes per node: 47.63 before the trim, 41.17 after** (target <= 40). Bytes/node target
   relaxed to the measured value (agent default, overnight rule); inline small literals is a
   possible later optimisation. The remainder is the 24-byte record (24 B/node) plus NUMBER
   literals: 278k NUMBERs x (24 B payload record + 64 B arena for an unsized decimal: a 24-byte
   `odin3_ast_number` plus 32 one-byte bits, rounded to 16) is about 12 B/node at 14% NUMBERs.
   The literal storage format is unchanged (a spec change). The real budget, mcml.v <= 20 MB, is
   met with 2x headroom. Not counted: `odin3_ast_get_number_str` (ABI) interns each literal's
   bit string in the design strtab for the design's life (the 1D text convention), so a plugin
   that reads every `NUMBER` of a large AST grows the strtab by about one byte per literal bit.

   **2A carry-overs that bind 2C/2D** (agent defaults ruled during 2A; final review 2026-10-10):
   - *2C lexer:* a `_` right after the base (`'h_F`) is a syntax error (Yosys and IEEE agree);
     the lexer rejects it, because `odin3_ast_number_parse` stays lenient on `_`.
   - *Printer canonical forms that the 2C parser must map back to equal trees*, so the
     parse–print–parse oracle holds: several `(* *)` groups print merged into one; a delay
     prints as `#(5)`; a `METACOMMENT` attribute prints as `// synopsys …` after the
     `case (…)` header; a system name prints raw (`$foo`); literal digits print normalised
     (lower case; `'sd5` prints as a plain signed decimal).
   - *Check rules derived from the normative §4.3/§4.4 tables* that 2C's builders must satisfy
     (`odin3_ast_check` rejects their violation): the flag fields hold enum values (DT ≤ `time`,
     NET ≤ the net-kind count, strength values in range, a charge strength in S0 only); LIST
     contents per slot (MODULE params `PARAM_DECL`, ports `PORT_DECL`/`PORT_REF`, INSTANTIATION
     params `CONNECTION`, FUNCTION/TASK ports `PORT_DECL` and locals D, SEQ/PAR locals D);
     a `CASE_ITEM` body is S under `CASE` and I under `GEN_CASE`; `CASE_ITEM`'s tail is empty
     exactly when `DEFAULT` is set, `EVENT_CONTROL`'s exactly when `STAR` is; a `HIER_NAME`
     part is an `IDENT` or a bit `SELECT` over an `IDENT`; a `UNIT` is never in a span; a
     `GEN_BLOCK` index only in the elaborated form; `SELECT`'s third slot is present exactly
     for a part select; in the elaborated form a `PORT_DECL` sits only in the port LIST.
   - *Rule 4 sees only this store:* an unnamed `INSTANCE` is legal under a `GATE_DECL` or under
     an `INSTANTIATION` of a `UDP_DECL` in the same store. A UDP declared outside the store
     (another read run, a library) makes the unnamed instance a check error; 2C/2D either keep
     the UDP in the store or name the instance.
   - *Plugin passes* cannot yet declare `wants_ast` or learn their pass-run ID through the ABI;
     an ABI addition is due before the slang adapter (Phase 5).
   - *`odin3_symtab_remap` is not yet called after `compact`* (spec §8): correct while no
     symbol table is filled; 2D wires it into the pass manager's `compact` for a design that
     keeps its symtab (see the `compact` row of `docs/PASSES.md`).
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
10. **2B declared divergences from both oracles** (spec §10.4, §13; logged with 2B Task 8,
    2026-10-10). In the first four cases Icarus and Yosys agree, and 2B differs from them on
    purpose; `pp-diff` expects 2B's behaviour there. The last case is a set of micros that
    Parmys rejects.
    - *Directives inside macro text* are `syntax: directive inside macro text` (PP-6). The cases
      are a conditional, `` `define ``, `` `undef `` or `` `include `` in a body (probes t5, u3).
      They also include a `` `define ``/`` `undef `` met while the actuals are collected, which
      both oracles process there. A conditional in an actual is processed only while it is
      collected from a file frame (probe w3).
    - *Recursion is judged on the spelling ancestry* (PP-7). The use is rejected even where the
      oracles terminate. In probe w1, `` `N(`KEEP) `` reaches `` `K ``, which uses `` `N ``
      again inside its own argument chain. Both oracles expand it to nothing.
    - *`translate_off` … `translate_on` is removed by the preprocessor* (PP-10). Icarus ignores
      the pair. Yosys drops the region in its lexer, after directives have been processed.
    - *A compiler directive name used as a macro name* is `syntax` (spec §7.3 row). For example,
      `` `define timescale 1 `` is rejected, and both oracles accept it.
    - *The splicing micros are Parmys-rejected* (PHASE2 #4). `syntax/preprocessor_define`,
      `preprocessor_complex_define` and `preprocessor_define_with_comment` rely on
      `` `ADDITION_num `` splicing a formal into a macro name. 2B reports `undefined macro
      'ADDITION_num'`. Yosys and the Parmys goldens fail the same way, while Icarus warns and
      substitutes nothing. The spec names only the last two; `preprocessor_define` fails
      identically.
