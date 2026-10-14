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
- [x] 2B project readers pass the corpus (Phase 2 formats); preprocessor
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
     ports; an internal tri-state net is a located error until the IR has a tri-state net model
     (superseded by #20: it becomes `$pmux` logic, as `tribuf -logic`);
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
   against 20 MB; measured by 2C's `--parse-only` run at 4.56 MB, entry 15); build, walk and
   check FULL (0.125 s with locations, against 0.300 s). The rule-1 location-decode cost inside check FULL is about
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
10. **2C spec approved** (`docs/specs/2026-10-10-2C-verilog-parser-design.md`; agent default under
   Peter's overnight rule, 2026-10-11, after an Opus review and two re-reviews verified with Bison
   3.8.2 toy grammars and a CMake test project): reentrant Flex lexer over the 2B stream (numbers as
   three tokens, per-directive argument shapes, metacomments attached at the CASE reduction); pure
   LALR(1) Bison with `%expect 0` and a committed allowlist of precedence-resolved conflicts checked
   against `bison --report=solved`; restricted lvalue nonterminals; one mixed item list for
   attributed declarations/statements; IEEE Table 5-4 precedence (`**` left, conditional `%prec '?'`);
   statement-form `VP`/`VP_ERR` action guards; error recovery capped at 20; generated sources in
   the build tree compiled inside `odin3_core` with `-Wno-*`-only source options. Open points
   decided as recommended: recovery on; `**` left; `-v`/`-y` libraries resolved by 2C; unnamed
   module instances are an error; generated code not committed. Declared divergences: attributes on
   connections and ANSI subroutine ports, unary operators on primaries only, SV named-argument calls
   (Parmys exceptions).
11. **2B declared divergences from both oracles** (spec §10.4, §13; logged with 2B Task 8,
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
    - *The include search never looks in the current directory* (PP-11; DESIGN §4.0, PHASE1
      #19; ruled at 2B Task 7). It tries the including file's directory, then the include
      directories in order. Icarus (`-E -I`) and Yosys (`read_verilog -I`) both search the
      current directory first and include its copy of the file, even when the file also exists
      in an include directory. `pp-diff` therefore runs the oracles from a neutral directory.
    - *`translate_off` … `translate_on` is removed by the preprocessor* (PP-10). Icarus ignores
      the pair. Yosys drops the region in its lexer, after directives have been processed.
      A `translate_off` comment inside a macro's actual is stripped to a space like any other
      comment there and opens no region (agent default). A later `translate_on` is then a stray
      `translate_on` warning, while Yosys's lexer would drop the region.
    - *A compiler directive name used as a macro name* is `syntax` (spec §7.3 row). For example,
      `` `define timescale 1 `` is rejected, and both oracles accept it.
    - *The splicing micros are Parmys-rejected* (PHASE2 #4). `syntax/preprocessor_define`,
      `preprocessor_complex_define` and `preprocessor_define_with_comment` rely on
      `` `ADDITION_num `` splicing a formal into a macro name. 2B reports `undefined macro
      'ADDITION_num'`. Yosys and the Parmys goldens fail the same way, while Icarus warns and
      substitutes nothing. The spec names only the last two; `preprocessor_define` fails
      identically.
12. **2B preprocessor results** (2B Task 9, 2026-10-10; spec §9, §10.3, §10.6).
    - *Caps.* `test_pp_limits` hits each of the 16 `ODIN3_PP_MAX_*` caps through
      `odin3_pp_test_set_limits`, and the 2^28 source-file cap at the root's `at` and at an
      `` `include `` (sparse files, so nothing is read). Each test asserts the exact located
      message. The log sink snapshots the guarded counter when the error is logged, which shows
      that nothing was created past the cap. `MAX_LOC_BYTES_PER_STREAM` is 2^29, per the spec §9
      amendment from Task 6's fix round.
    - *Stacks.* The stream's own file now leaves the include stack when its frame ends. Frames,
      runs, conditionals and the include stack are therefore empty after every stream; `test_pp_mixed`
      asserts this (agent default).
    - *Micro sweep* (`test_pp_check`, smallest first, a fresh preprocessor per file,
      `odin3_pp_check` on every OK stream):
      - `tests/micro/verilog/preprocessor`: all 22 files pass, including `nested_ifdef.vh`.
      - `syntax/*define*`: the 3 splicing micros stop at `undefined macro 'ADDITION_num'`
        (entry 11).
      - Whole tree, 610 `.v`/`.vh` files (`keywords/.generic` included): 589 pass and check;
        21 stop with a listed error. These are the 3 splicing micros, plus 18 files that use
        macros from their includer:
        - the 9 module files of the four multi-file `syntax/*-syntax` micros (`BITS`);
        - the 4 `keywords/.generic` templates (`WIDTH`);
        - `keywords/assign/assign.vh`, `keywords/for/generate_for.vh` and three
          `generate_endgenerate/*.vh` headers.
      - 16 of those 18 are read in context by an OK run of their includer (asserted). The other
        two, `ifdef-else-syntax/module_a_or.v` and `ifndef-else-syntax/module_a_and.v`, are the
        variants that their includer's conditional does not select.
    - *Final-review fix round* (2026-10-10; `final-review-2B2.md`):
      - *I1:* the lexical scanners (string, escaped identifier, comment, `` `line `` file name)
        read the frame through a run iterator, O(1) amortised per byte. Before, a comment or
        escaped identifier spread over many runs cost O(len × runs): a 600 KB input ran past
        120 s. `test_pp_expand` counts run hops over 10,000 runs and fails on the old scanners.
      - *I2:* new cap `ODIN3_PP_MAX_SRCMAN_BYTES_PER_STREAM` = 2^27 (agent default, spec §9).
        It is charged 4 B per registered line start and 8 B per segment, before they are added,
        and tested with a newline-header include bomb and a `/**/` body bomb.
      - *M4:* the text buffer starts at the stream file's size and never grows past
        `MAX_STREAM_BYTES` + 1. This needed a new exact `odin3_strbuf_reserve` in `util/str`.
      - *Other minors fixed:* a NUL right after a bracket in an actual (M1); the actuals' runs
        now count as live while a body is substituted (M3); the cache-entry comment (M5); cap
        errors on inserted bytes are now located at the innermost macro use.
      - *Left:* M2 (the recursion walk does a binary search per ancestor: bounded by the chain
        cap) and M6 (newlines between a macro name and its `(` are consumed with the use).
      - *Oracles disagree (M7, for `pp-diff`'s exclusion list):*
        - a comment between a macro name and `(`: Icarus expands, Yosys and 2B reject;
        - a newline before `(`: Icarus and 2B accept, Yosys rejects;
        - `` `line `` inside a macro body: Yosys applies it, Icarus and 2B drop it;
        - a `//` comment ending a `` `define `` with a trailing `\`: Yosys and 2B end the
          text, Icarus continues it;
        - formals inside body strings (§10.4);
        - a comment in a formal default (`` `define A(x=/*c*/ 3) [x] ``): Icarus keeps the
          comment text in the expansion, Yosys and 2B drop it. The tokens are identical, so
          pp-diff reports agree, not oracles disagree; the fixture
          `tests/tools/fixtures/pp_diff/default_comment.v` pins it against both oracles (2B-3
          final review).
    - *Benchmark* (`build/release/bench_pp`, Release, dev machine (i7-13700, WSL2), 2026-10-10,
      `$VTR_ROOT/vtr_flow/benchmarks/verilog`, after the fix round, output verbatim):

      ```
      file (smallest first)          bytes in      out   lines    segs   bufs  defs macros  srcman B pp heap B heap/in  best ms check
      single_wire.v                        62       62       4       1      1     0      0      1040     67133 1082.79    0.002 ok
      single_ff.v                          96       96       6       1      1     0      0      1040     67201  700.01    0.002 ok
      and_latch.v                         260      260      22       1      1     0      0      1136     67529  259.73    0.002 ok
      multiclock_separate_and_latch.v      702      702      54       1      1     0      0      1264     68413   97.45    0.003 ok
      multiclock_output_and_latch.v       889      889      64       1      1     0      0      1264     68787   77.38    0.004 ok
      multiclock_reader_writer.v         1021     1021      62       1      1     0      0      1264     69051   67.63    0.004 ok
      diffeq1.v                          1409     1409      57       1      1     0      0      1264     69827   49.56    0.005 ok
      diffeq2.v                          2131     2131      64       1      1     0      0      1264     71271   33.44    0.006 ok
      ch_intrinsics.v                    4838     4283     293      36     16     5      5      4272     77309   15.98    0.019 ok
      ch_intrinsics_nolatches.v          5005     4450     302      36     16     5      5      4272     77643   15.51    0.018 ok
      stereovision3.v                   35847    35847    1062       1      1     0      0      9200    138703    3.87    0.059 ok
      sha.v                             38061    37865    2172      38     15     9      9     18608    144139    3.79    0.093 ok
      mkPktMerge.v                      39878    39690    1536     161     79     4      4     15024    148717    3.73    0.168 ok
      blob_merge.v                      58879    55660    1618    1003    475    54     54     33456    194367    3.30    0.344 ok
      stereovision2.v                   63099    61774    1847     270    110    51     51     17072    198151    3.14    0.195 ok
      spree.v                           68099    65433    3073     392    160    73     73     29360    212599    3.12    0.295 ok
      bgm.v                             71508    71298    2664      93     46     2      2     20144    211177    2.95    0.258 ok
      boundtop.v                        72116    72116    2826       1      1     0      0     17392    211241    2.93    0.192 ok
      boundtop_nolatches.v              81343    81343    3054       1      1     0      0     17392    229695    2.82    0.212 ok
      raygentop.v                       84801    84801    2979       1      1     0      0     17392    236611    2.79    0.200 ok
      raygentop_nolatches.v             92241    92241    3179       1      1     0      0     17392    251491    2.73    0.228 ok
      stereovision1.v                  102964   102964    2554       1      1     0      0     17392    272937    2.65    0.217 ok
      stereovision0.v                  131550   131550    3906       1      1     0      0     17392    330109    2.51    0.287 ok
      or1200.v                         135776   105416    5238    2022    750   524    383     82608    397969    2.93    0.907 ok
      mkSMAdapter4B.v                  137869   137559    4349     158     76     8      8     39600    344763    2.50    0.490 ok
      LU8PEEng.v                       140203   132207    5034    2121    969   182    164     98992    372599    2.66    0.825 ok
      LU32PEEng.v                      155750   147942    5498    2313   1065   182    164    131760    415981    2.67    0.891 ok
      LU64PEEng.v                      176532   168880    6044    2569   1193   182    164    131760    457545    2.59    0.979 ok
      mkDelayWorker32B.v               204602   204056    6629     329    160    11     10     45744    480117    2.35    0.739 ok
      arm_core.v                       346864   336469    8212      43     13     7      7     67248    761457    2.20    1.089 ok
      mcml.v                           650837   649054   24508    5590   2776    39     39    328368   1421051    2.18    2.751 ok
      set: 31 files, 2905232 bytes in, 0.011 s (sum of best of 5; target <= 0.5 s: ok); pp heap over 3 x input in 16 files
      mcml.v: 2.751 ms (target <= 50 ms: ok); pp heap 1421051 B = 2.18 x input (target <= 3 x: ok), srcman 328368 B beside it
      max RSS: 4064 KiB
      ```

      Targets met: mcml.v 2.8 ms against 50 ms; the VTR set 0.011 s against 0.5 s; mcml.v's
      preprocessor heap is 2.18 x its input (2.79 x before the text buffer started at the
      file's size), with the srcman's 328 KB beside it.

      Per file, the ruling is <= 3 x input + 128 KiB. All 31 files are within it. The 16 files
      above 3 x are small files dominated by the fixed ~67 KB: the 64 KiB macro-arena chunk
      that `odin3_pp_create` allocates, plus the struct and vectors.

      The spec's "51 `define`s" for mcml.v counts 12 commented-out lines; 39 definitions are
      made. mcml.v needs 2,776 buffers (the spec estimated about 5,000).
    - *pp-diff* (2B Task 12, 2026-10-11; spec §10.4; `tools/pp-diff`, CTest `pp_diff`, label
      `differential`, exit 77 and skipped without `iverilog`, Yosys or the VTR set, so not run
      in CI). Odin III's `preprocess` is compared with `iverilog -E -g2005 -grelative-include`
      and with Yosys `read_verilog -nosynthesis -ppdump`.
      - *How it runs:* Odin III runs in its own directory. The oracles run from a separate empty
        directory. `-I` directories are made absolute first. All three outputs are normalized
        alike (PP-10 regions; pass-through, `` `line ``, `` `file_push `` and `` `file_pop ``
        lines) and compared as tokens.
      - *What the normalization hides:* normalizing Odin III's stream would hide a PP-10 or
        pass-through bug. So its raw stream must also hold no `translate_off` comment.
        Pass-through is covered by `test_pp_directives`.
      - *Failure rules:*
        - Odin III rejects only by exit 1 with an `error: kind: detail` line. A signal, a
          sanitizer abort, a timeout, any other exit code or a missing diagnostic is CRASH.
          So is an oracle signal or timeout.
        - Icarus rejects by a nonzero exit or by an error line on stderr, since it exits 0
          after some errors.
        - Where the oracles disagree, the table `ORACLES_DISAGREE` names the oracle Odin III
          must match. A file with no row in the table, or one matching the other oracle,
          fails.
      - *Input set:* `pp-diff --suite` takes 287 files, smallest first, mcml.v last: 20 reviewer
        probes (`rec.v` included), 2 fixtures, 234 micros that hold a backtick and are not
        included by another micro, and 31 VTR files. The debug build runs it in 10 s, exit 0.
      - *Result:* agree 271, oracles disagree 11 (all as the table expects), declared 5,
        excluded 20. DIFFER, CRASH, DECLARED NOT MET and DISAGREE NOT EXPECTED are all 0.
        - The 271 are 8 probes (in v2 all three tools reject), the fixture
          `default_comment.v`, 231 micros, and all 31 VTR files
          (mcml.v: 133,191 tokens). The four `translate_off` benchmarks are among them.
      - *Declared:* the Odin III error is expected, and both oracles must accept alike.
        - probes t5 and u3: `directive inside macro text`;
        - probe w1: `recursive macro 'N'`;
        - the fixture `tests/tools/fixtures/pp_diff/directive_name.v` (`` `define timescale 1 ``);
        - `rec.v`: `recursive macro 'R'`. Odin III runs alone here, because both oracles loop
          forever on it.
      - *Oracles disagree.* Each row below gives the oracle Odin III must match:
        - Yosys:
          - t8 and the 3 splicing micros: Icarus substitutes nothing for an undefined macro;
          - t6: Icarus substitutes formals in a string;
          - w2: `` `define F() ``, which Icarus rejects;
          - t4: a `//` comment ending in `\`, after which Icarus continues the `define`;
          - t7: a comment inside a body, where Icarus pastes `ab`.
        - Icarus:
          - t9 and w5: a newline before `(`, which Yosys rejects;
          - u1: a multi-line `/* */` in a `define`. Icarus reports it, as Odin III does;
            Yosys keeps it.
      - *Excluded:* the 20 micros that another micro includes. They use their includer's
        macros, so each is decided by Odin III's `files-read.txt`:
        - 18 are compared through an includer.
        - `ifdef-else-syntax/module_a_or.v` and `ifndef-else-syntax/module_a_and.v` are the
          variants that their includer's conditional does not select, so no tool reads them.
      - *Agent defaults:*
        - `-grelative-include`, so Icarus searches the including file's directory first, as
          Odin III (PP-11) and Yosys do.
        - `-nosynthesis`, so Yosys does not define `SYNTHESIS`.
        - A `` `NAME `` left in Yosys's dump counts as a Yosys rejection, because its lexer
          reports "undefined macro" there. A Yosys parse error after the dump does not count.
        - A declared divergence is checked by a substring of Odin III's error.
        - A pass-through directive is stripped from its backtick to the end of its line.
        - Based literals are compared without their inner blanks.
        - No C fix was needed.
    - *2B-3 final-review fix round* (2026-10-11; `final-review-2B3.md`):
      - *I1:* `fuzz_pp_smoke` passes `-print_funcs=0`. Without an unversioned `llvm-symbolizer`,
        libFuzzer's `NEW_FUNC` lines made ASan fall back to `addr2line` and stalled the 10 s
        smoke for about 90 s, against a 120 s timeout. It now takes 11 s. CMake also sets
        `ASAN_SYMBOLIZER_PATH` to a versioned `llvm-symbolizer` when one is found, so a crash
        report stays readable.
      - *I2:* unlocated errors no longer print `odin3: error: odin3: error: …`. The seven call
        sites log `<kind>: <detail>` alone and the log sink supplies the prefix. The corpus
        harness requires the `odin3: error: ` prefix and reports a doubled one.
      - *Minors fixed:*
        - the corpus README covers `compare`, `project_corpus` and `pp_properties`;
        - a dumped `output` without `path` is reported, not a traceback;
        - `dump_project ""` reports an empty path;
        - `read_project --revision` refuses a value starting with `--`;
        - `preprocess` logs `preprocess: no Verilog design files`;
        - an OOM test covers `odin3_pass_set_revision`;
        - `test_project_pass` pins the whole `0.segments.json`;
        - pp-diff refuses a path holding `"` (exit 2);
        - the `default_comment.v` fixture (M7 list above);
        - `preprocess --out` is documented to expect a fresh directory.
      - *Kept parked:* `project_pass.c` reading srcman internals, the loose Icarus error regex,
        and srcman-raised paths under `--diag-paths resolved`. Each waits for the 2A srcman
        touch or the next pp-diff change.
    - *2B-4 Odin II XML reader* (2B Task 14, 2026-10-11; spec §5.4 PRJ-15; `xml.c`, `odin2.c`):
      `.xml` entries and `--format odin2` read Odin II configs; `project_corpus` runs the
      `odin2` format (`LANDED_FORMAT_PHASES` holds `odin2-reader`): 10 cases ok, the 4 whose
      error needs the front end skipped as in the other formats. Agent defaults:
      - a new cap `ODIN3_PROJ_MAX_XML_NODES` = 2^20 elements, attributes and text runs per XML
        file, checked before each is stored (a 2^28-byte file of `<a/>` would otherwise need
        about 3 GB; real configs hold under 100 nodes); it belongs in spec §9's table;
      - the XML subset follows expat where the corpus is silent: a UTF-8 BOM and an XML
        declaration at the start are skipped (its encoding ignored: UTF-8), newlines in text
        and attribute values are normalized, C0 controls, invalid UTF-8, U+FFFE/U+FFFF and
        `]]>` in text are `syntax`; lines count LF only (the source manager's lines), where
        expat also counts a lone CR;
      - `input_type` and `output_type` are lower-cased in ASCII only (Python's `str.lower()`
        also folds non-ASCII letters); values are stripped as `str.strip()`
        (`odin3_proj_py_strip`, now shared with `.o3proj` map cells).
13. **2D spec approved** (`docs/specs/2026-10-11-2D-elaboration-design.md`; agent default under
   Peter's overnight rule, 2026-10-11, after a Fable review (2 critical, 10 important, 17 minor,
   all applied in v2) and a Fable re-review that approved v2 (minors N1–N7 applied in v3, N4 kept
   because `hierarchy` is a 1D built-in)): two stages with the elaborated AST as the seam (stage 1
   Verilog-only: lookup, top, parameters, `defparam`, specialization, generate, constant
   functions; stage 2 shared with slang: binding, Yosys `genrtlil` widths and signedness, drivers,
   memories, processes, inlining, initial values); `write_rtlil` as the bridge to Yosys.
   **Parmys's Yosys 0.55 is the semantic oracle** for every verified claim, the constant table,
   the bridge and the exit test; Yosys 0.69 only as a nightly drift check.
   - **IR change A — agent default for Peter's review:** nine registry cells with Yosys simlib
     semantics, `$neg $xnor $reduce_xnor $reduce_bool $logic_not $logic_and $logic_or $shiftx
     $pow` (no `$shift`: nothing emits it).
   - **IR change B — agent default for Peter's review:** processes are `$proc` cells with real
     `I`/`O` ports and an opaque `BODY` (`ODIN3_VAL_PROC`; positions are pin indices, constants are
     pins on the shared constant nets), validated by `verify`; IR-3, IR-4, IR-6, IR-15 and check
     rule 4 unchanged; passes declare `processes` (default `REFUSE`), so `opt` runs only after
     `proc`.
   - **IR change C — agent default for Peter's review:** `odin3_port_spec` gains `has_range`,
     `msb`, `lsb`, `is_signed` (the sign is internal to `odin3_port_def`, not the frozen ABI port
     struct).
   - Decisions as ruled (spec §20): 1 `$proc` cell (above); 2 nine cells (above); 3 port range and
     sign (above); 4 `--nolatches` off by default, passed by the 2H harness and the VTR scripts as
     Parmys does; 5 parameter precedence project > named `#()` > first `defparam` > ordered `#()`
     (Yosys, verified), warning when a `defparam` loses; 6 DESIGN §4.0's top rule kept, the 2H
     micro harness passes `--top` from the Parmys golden; 7 undefined modules are
     `unresolved_module`, `--auto-blackbox` opt-in; 8 Yosys's mem2reg-as-needed rules and one
     collected `$mem` per remaining array with raw-index ports; 9 `write_rtlil` in 2D; 10 a failed
     stage 2 leaves valid but incomplete modules; 11 Parmys leniency with warnings (implicit nets
     anywhere, unnamed-block declarations, unsized constants in concatenations); 12 modules not
     reachable from the top are not elaborated; 13 don't-cares (§14.6): x kept explicit through
     2D, Yosys's x rules mirrored in 2F, surviving x written as Parmys's `unconn` (= 0; all 934
     goldens declare it, 314 use it), and `equiv-check --x-report` in 2H.
   - **Exit-bar ruling on x (decision 13):** 2H reports identical / equivalent / equivalent
     outside x-cones / differs; "equivalent outside x-cones" does not count toward PHASE2 #4 by
     default, and counts per design only when its excluded outputs are reviewed and recorded in
     the exceptions list (agent default; Peter may override). The defined-output miter of spec
     §14.6 item 5 is the follow-up in 2H's backlog.
   - **Amendment (2D spec v4.2, EL-33):** the eight opt passes are `processes = OK` (`$proc`
     opaque; `opt_muxtree`/`opt_clean` skip modules with processes), so Parmys's pre-`proc` opts
     run in Parmys's order (2F mode P) — agent default for Peter's review; Fable-verified:
     pre-proc opt order changes netlists, stereovision3 812 vs 792.
14. **Peter confirmed the agent defaults** (2026-10-11: "For all the questions - sounds good and
   your ideas decisions seem fine"): the 2D rulings of #13 including IR changes A, B and C and
   decision 13 (x-report), the EL-33 mode P and EL-35/36 chunking amendments (2D spec v4.2),
   IR change D (eleven FF/latch cells, 2E-1), the 2E decisions 1–8 and 2F decisions 1–7 as
   ruled after the joint Fable review, and the S-P exit splice (Odin III through `proc`, Yosys
   0.55 from `fsm` on).
15. **2C results and declared divergences** (2C Task 10, 2026-10-12; spec §7.4, §7.6, §9).
   - *Form.* VP-11: pure LALR(1) Bison (`lr.type lalr`, `%expect 0`, no `%expect-rr`), zero
     reported conflicts in 947 states; the conflicts Bison resolves by precedence equal the
     committed allowlist `tests/verilog/solved_conflicts.txt` (731 lines: the operator pairs and
     the two dangling-`else` lines, `verilog_grammar_solved`). VP-13: every binary operator is
     left-associative, `**` included (Table 5-4 and Yosys's `%left OP_POW`; Odin II's `%right`
     differs). VP-14: recovery at `;`/`endmodule`, actions inert after the first error, a cap of
     20 errors per run (`too many errors; stopping`). VP-21: Bison/Flex output generated into
     `build/<preset>/generated/verilog`, never committed, compiled inside `odin3_core`; the
     measured per-file list is one flag, `-Wno-unused-but-set-variable` on the parser
     (`yyparse`'s `yynerrs` under `api.pure`, clang only) — none of the expected
     `-Wno-conversion`/`-Wno-sign-*`/`-Wno-unused-parameter`/`-Wno-missing-prototypes` occur.
   - *Sweeps* (`tools/parse-check/parse-corpus`, `odin3-parse-check` per file with the location
     oracle and the print/re-parse/equal round trip, smallest first, one file at a time, Release,
     in `odin3-heavy.slice`; CTests `verilog_micro_parse`, `verilog_vtr_parse`,
     `verilog_writer_parse`, label `sweep`):
     - micros: 539 files, 489 pass, 50 expected failures (`tests/verilog/micro_expect_fail.txt`,
       each with its reason), 0 unexpected. Of the 56 that failed before Task 10, 6 were a 2C
       gap, fixed (binary `~&`/`~|`, below); 38 are rejected by Parmys too (8 intended `*_failure`
       negatives, 14 SystemVerilog named-argument calls, the 3 splicing micros of entry 11, 13
       fragments that need a macro only their includer defines); 12 are illegal Verilog-2005 in
       a micro meant to pass (10 with `$display` as a module item, the 5 `nested_ifdef_*`
       among them; `recursive_task`'s `task automatic integer … endfunction`; `fir_scu_rtl`'s
       module instance inside `always`), and Parmys rejects them as well — at the same line,
       except the 5 `nested_ifdef_*`, whose Parmys goldens failed earlier, on the include path
       (`Can't open include file`, an oracle working-directory artifact). Every one of the 50
       has a failed Parmys golden.
     - VTR: 64 files (`verilog/*.v` 31 and `koios/*.v` 33, `koios/lenet.v` last), 64 pass in
       7.5 s; the largest round trip, `odin3-parse-check koios/lenet.v`, peaks at 224,224 KiB.
     - writer round trip (Review Focus 1): the 12 committed `tests/golden/blif` BLIFs pass.
       Over the whole golden set on the dev machine (1,846 BLIFs, both architectures, 580 s,
       peak 3.4 GB) 1,840 pass; the 6 others are written as 317 MB–2.1 GB of Verilog
       (`LU64PEEng.parmys` ×4, `LargeRam.odin` ×2) and stop at the 2B source-file cap
       (`file … is N bytes, over the limit of 268435456`), by design.
   - *Bench* (`build/release/bench_ast --parse --set-target $VTR_ROOT/vtr_flow/benchmarks/verilog/*.v`,
     Release, dev machine, 2026-10-12 (rerun at fix round 1), output verbatim; each file in a child process of its own,
     so peak RSS is per file; `pp` is the 2B preprocessor, `parse` the 2C parser and
     `odin3_ast_finish`, `check` is check FULL, `read_verilog` is the pass's reader end to end on
     a fresh design). This replaces entry 8's "an estimate until 2C's `--parse-only` run":
   ```
   single_wire.v                           62 B (stream        62)  nodes       11  comments      0  ast     163908 B  srcman     1040 B  strtab +     6 (    123 B)  pp 0.000 s  parse 0.000 s  check 0.000 s  read_verilog 0.000 s  peak RSS 1920 KiB
   single_ff.v                             96 B (stream        96)  nodes       16  comments      0  ast     163932 B  srcman     1040 B  strtab +     7 (    126 B)  pp 0.000 s  parse 0.000 s  check 0.000 s  read_verilog 0.000 s  peak RSS 1864 KiB
   and_latch.v                            260 B (stream       260)  nodes       26  comments      2  ast     164052 B  srcman     1136 B  strtab +     8 (    138 B)  pp 0.000 s  parse 0.000 s  check 0.000 s  read_verilog 0.000 s  peak RSS 1864 KiB
   multiclock_separate_and_latch.v        702 B (stream       702)  nodes       84  comments      3  ast     164688 B  srcman     1264 B  strtab +    13 (    226 B)  pp 0.000 s  parse 0.000 s  check 0.000 s  read_verilog 0.000 s  peak RSS 1864 KiB
   multiclock_output_and_latch.v          889 B (stream       889)  nodes      112  comments      3  ast     164940 B  srcman     1264 B  strtab +    15 (    239 B)  pp 0.000 s  parse 0.000 s  check 0.000 s  read_verilog 0.000 s  peak RSS 1864 KiB
   multiclock_reader_writer.v            1021 B (stream      1021)  nodes      182  comments      2  ast     165892 B  srcman     1264 B  strtab +    11 (    221 B)  pp 0.000 s  parse 0.000 s  check 0.000 s  read_verilog 0.000 s  peak RSS 1864 KiB
   diffeq1.v                             1409 B (stream      1409)  nodes      186  comments      2  ast     165820 B  srcman     1264 B  strtab +    19 (    219 B)  pp 0.000 s  parse 0.000 s  check 0.000 s  read_verilog 0.000 s  peak RSS 1864 KiB
   diffeq2.v                             2131 B (stream      2131)  nodes      115  comments      3  ast     165088 B  srcman     1264 B  strtab +    12 (    165 B)  pp 0.000 s  parse 0.000 s  check 0.000 s  read_verilog 0.000 s  peak RSS 1864 KiB
   ch_intrinsics.v                       4838 B (stream      4283)  nodes      520  comments      2  ast     169632 B  srcman     4272 B  strtab +    48 (    552 B)  pp 0.000 s  parse 0.000 s  check 0.000 s  read_verilog 0.000 s  peak RSS 1864 KiB
   ch_intrinsics_nolatches.v             5005 B (stream      4450)  nodes      522  comments      2  ast     169752 B  srcman     4272 B  strtab +    48 (    562 B)  pp 0.000 s  parse 0.000 s  check 0.000 s  read_verilog 0.000 s  peak RSS 1864 KiB
   stereovision3.v                      35847 B (stream     35847)  nodes     2304  comments     11  ast     190940 B  srcman     9200 B  strtab +    64 (    759 B)  pp 0.000 s  parse 0.001 s  check 0.000 s  read_verilog 0.001 s  peak RSS 1992 KiB
   sha.v                                38061 B (stream     37865)  nodes     5486  comments     65  ast     298508 B  srcman    18608 B  strtab +    61 (    386 B)  pp 0.000 s  parse 0.001 s  check 0.000 s  read_verilog 0.001 s  peak RSS 2120 KiB
   mkPktMerge.v                         39878 B (stream     39690)  nodes     2566  comments    371  ast     194952 B  srcman    15024 B  strtab +   125 (   1328 B)  pp 0.000 s  parse 0.001 s  check 0.000 s  read_verilog 0.001 s  peak RSS 1992 KiB
   blob_merge.v                         58879 B (stream     55660)  nodes     5229  comments    359  ast     387796 B  srcman    33456 B  strtab +   215 (   2774 B)  pp 0.000 s  parse 0.001 s  check 0.001 s  read_verilog 0.002 s  peak RSS 2248 KiB
   stereovision2.v                      63099 B (stream     61774)  nodes     7277  comments     75  ast     410760 B  srcman    17072 B  strtab +   460 (   7276 B)  pp 0.000 s  parse 0.002 s  check 0.001 s  read_verilog 0.002 s  peak RSS 2376 KiB
   spree.v                              68099 B (stream     65433)  nodes     6300  comments    206  ast     399088 B  srcman    29360 B  strtab +   447 (   4637 B)  pp 0.000 s  parse 0.002 s  check 0.001 s  read_verilog 0.002 s  peak RSS 2376 KiB
   bgm.v                                71508 B (stream     71298)  nodes     8920  comments    392  ast     520768 B  srcman    20144 B  strtab +   439 (   3659 B)  pp 0.000 s  parse 0.002 s  check 0.001 s  read_verilog 0.002 s  peak RSS 2476 KiB
   boundtop.v                           72116 B (stream     72116)  nodes     8456  comments     55  ast     520360 B  srcman    17392 B  strtab +   476 (   3899 B)  pp 0.000 s  parse 0.002 s  check 0.001 s  read_verilog 0.002 s  peak RSS 2468 KiB
   boundtop_nolatches.v                 81343 B (stream     81343)  nodes     8894  comments     86  ast     523820 B  srcman    17392 B  strtab +   476 (   3909 B)  pp 0.000 s  parse 0.002 s  check 0.001 s  read_verilog 0.002 s  peak RSS 2504 KiB
   raygentop.v                          84801 B (stream     84801)  nodes     8771  comments     33  ast     526884 B  srcman    17392 B  strtab +   364 (   2963 B)  pp 0.000 s  parse 0.002 s  check 0.001 s  read_verilog 0.002 s  peak RSS 2632 KiB
   raygentop_nolatches.v                92241 B (stream     92241)  nodes     8770  comments    138  ast     526292 B  srcman    17392 B  strtab +   324 (   2586 B)  pp 0.000 s  parse 0.002 s  check 0.001 s  read_verilog 0.002 s  peak RSS 2632 KiB
   stereovision1.v                     102964 B (stream    102964)  nodes    11559  comments     16  ast     611604 B  srcman    17392 B  strtab +   553 (   8539 B)  pp 0.000 s  parse 0.003 s  check 0.001 s  read_verilog 0.003 s  peak RSS 2748 KiB
   stereovision0.v                     131550 B (stream    131550)  nodes    18680  comments     39  ast    1036376 B  srcman    17392 B  strtab +  1222 (  14976 B)  pp 0.000 s  parse 0.004 s  check 0.001 s  read_verilog 0.004 s  peak RSS 3416 KiB
   or1200.v                            135776 B (stream    105416)  nodes    10738  comments   1423  ast     641104 B  srcman    82608 B  strtab +   713 (   9844 B)  pp 0.001 s  parse 0.003 s  check 0.001 s  read_verilog 0.004 s  peak RSS 3044 KiB
   mkSMAdapter4B.v                     137869 B (stream    137559)  nodes     9102  comments    756  ast     530676 B  srcman    39600 B  strtab +   765 (  15737 B)  pp 0.001 s  parse 0.003 s  check 0.001 s  read_verilog 0.003 s  peak RSS 2748 KiB
   LU8PEEng.v                          140203 B (stream    132207)  nodes    17376  comments    582  ast    1035960 B  srcman    98992 B  strtab +   890 (   9809 B)  pp 0.001 s  parse 0.004 s  check 0.002 s  read_verilog 0.005 s  peak RSS 3536 KiB
   LU32PEEng.v                         155750 B (stream    147942)  nodes    20098  comments    582  ast    1134696 B  srcman   131760 B  strtab +  1009 (  10728 B)  pp 0.001 s  parse 0.005 s  check 0.002 s  read_verilog 0.006 s  peak RSS 3668 KiB
   LU64PEEng.v                         176532 B (stream    168880)  nodes    23648  comments    582  ast    1342168 B  srcman   131760 B  strtab +  1169 (  12008 B)  pp 0.001 s  parse 0.005 s  check 0.003 s  read_verilog 0.006 s  peak RSS 3928 KiB
   mkDelayWorker32B.v                  204602 B (stream    204056)  nodes    13372  comments   1247  ast     745824 B  srcman    45744 B  strtab +   958 (  19056 B)  pp 0.001 s  parse 0.004 s  check 0.001 s  read_verilog 0.005 s  peak RSS 3368 KiB
   arm_core.v                          346864 B (stream    336469)  nodes    22852  comments   2062  ast    1278348 B  srcman    67248 B  strtab +  1004 (  12544 B)  pp 0.001 s  parse 0.006 s  check 0.002 s  read_verilog 0.007 s  peak RSS 4316 KiB
   mcml.v                              650837 B (stream    649054)  nodes    78229  comments   2985  ast    4235356 B  srcman   328368 B  strtab +  5614 (  55033 B)  pp 0.003 s  parse 0.017 s  check 0.009 s  read_verilog 0.020 s  peak RSS 9020 KiB
   mcml.v AST + srcman               :   4563724 B  (target <= 20,000,000 B: ok)
   mcml.v parse-only (read_verilog)  :     0.020 s  (pp 0.003 s + parse 0.017 s; target <= 1.000 s: ok)
   31 files given, read_verilog total:     0.085 s  (pp + parse 0.088 s; set target <= 5.000 s: ok)
   ```

     Targets met: mcml.v AST + srcman 4.56 MB (≤ 20 MB; 78,229 nodes, against 2A's 200k-node
     estimate of 9.5 MB), mcml.v parse-only 0.020 s (≤ 1 s), the 31 files 0.085 s (≤ 5 s); a
     missed target prints `MISSED` and exits 1. 2B's
     own §10.6 numbers for the preprocessor are in entry 12. Koios (reported, not targeted,
     point (n); no `--set-target`), last lines verbatim of
     `bench_ast --parse $VTR_ROOT/vtr_flow/benchmarks/verilog/koios/*.v`:
   ```
   lenet.v                           11062147 B (stream  11059985)  nodes   919695  comments    602  ast   34120584 B  srcman  1049648 B  strtab + 25042 ( 735092 B)  pp 0.046 s  parse 0.198 s  check 0.065 s  read_verilog 0.221 s  peak RSS 75416 KiB
   33 files given, read_verilog total:     0.830 s  (pp + parse 0.906 s)
   ```
   - *Declared divergences* (spec §9, verbatim): attributes on `assign` accepted; attributes on
     a port connection, on an ANSI function/task port and on a function call are syntax errors
     (Annex A allows them); unary operators bind to a primary only (`- -a` needs parentheses;
     Odin II and Yosys accept it); SystemVerilog named-argument calls `f(.a(x))` are syntax
     errors — the `syntax/*_expression_in_{function,task}_port.v` micros become Parmys
     exceptions under PHASE2 #4; `input reg` parsed without comment (D5 is 2D's); `localparam`
     and `#()` accepted in a parameter port list; `unsigned` accepted everywhere `signed` is; a
     UDP-style instance with a drive strength or a `#delay2` is a syntax error (§3.1 i).
     Added by Tasks 4–10 (agent defaults):
     - *binary `~&` and `~|` are accepted* (Task 10): A.8.6 has them only as unary operators,
       but Yosys and Odin II accept them and six micros with Parmys goldens use them
       (`operators/binary_nand`, `binary_nor`, `full/ansiportlist`, `ansiportlist_2`, `binops`,
       `syntax/bm_log_all`). They take the precedence of `&` and `|` (Table 5-4's rows, Yosys's
       `%left`) and read as Yosys reads them, UNARY[`~`] over BINARY[`&`]/BINARY[`|`] at the
       left operand, so the printer writes `~(a & b)`; no 2A sub-kind or ABI change (2C adds
       none). The alternative, BINARY sub-kinds `~&`/`~|`, would be an ABI addition for 2D's
       ABI round. One difference from Yosys remains: an attribute after the operator
       (`a ~& (* k *) b`) is attached to the inner BINARY, where Yosys attaches it to the outer
       `AST_BIT_NOT`. Pinned by coverage row E1 (`E1_bitwise.v`, module `e1_binary_nand_nor`);
     - *stricter than Parmys's `read_verilog -sv`* (Annex A, no tree change): a `casez`/`casex`
       generate (`generate casez (W) … endcase` is `syntax: unexpected casez`; A.4.2 has only
       `case`; Yosys accepts it with or without `-sv`), and implicit port connections `.*`
       (`'.*' (implicit port connections) is SystemVerilog, not Verilog-2005`, spec §3.1; Yosys
       accepts it under `-sv`, the Parmys mode). A design with either form reads in Parmys and
       not in Odin III;
     - *a stray `;` module item is accepted with a warning (PHASE2 #16)* (Task 11, 2C-2 final
       review; Peter's ruling): `wire a;;` or a lone `;` between items makes no node and logs
       `<file>:<line>:<col>: warning: empty module item ';' ignored`, as Yosys 0.55 (with or
       without `-sv`) and Icarus accept it, though Annex A has no empty module item. The rule is
       in `module_items`, so a generate region's and a generate block's items take it too (Yosys
       accepts both); a `;` at unit level, in a port list or in a declarator list stays a syntax
       error. No corpus file has one;
     - *more lenient than Parmys*: `macromodule` is a module (1364-2005 §12.1); Yosys has no
       such keyword, so every `keywords/macromodule` micro fails in Parmys at its `macromodule`;
     - *lenient forms 2D judges* (parse-lenient, no tree change): module items of any kind in a
       `generate` region or block (a port declaration, `parameter`, `specify`, a nested
       `generate`); `m #() u (…)` (an empty params LIST; Annex A needs one assignment); ordered
       and named parameter values mixed; strengths on MOS and pass switches; a single pull
       strength of either polarity; null loop bodies; `reg x = 1;` as a block-item declaration;
       `a.b[0]` as a non-ANSI port reference; `input integer [3:0]` and
       `parameter integer [3:0]`; `(highz0, highz1)`.
   - *Parmys exceptions added to PHASE2 #4* (micros Parmys rejects; for those 2C rejects too,
     the reasons are in `tests/verilog/micro_expect_fail.txt`): the 14 SystemVerilog
     named-argument micros, `syntax/{basic,bitwise_op,conditional,mix}_expression_in_{function,task}_port.v`
     and `syntax/instantiated_by_name_{function,task}_{valid,invalid,invalid_1}.v`; and the 12
     illegal-Verilog-2005 micros above; and the 3 `keywords/macromodule` micros 2C reads
     (`macromodules_modules`, `multiple_macromodules`, `multiple_topmodules`: Yosys has no
     `macromodule`). The 8 `*_failure` micros, the 3 splicing micros (entry 11) and the 13
     includer-macro fragments are not designs of their own.
   - *Known, not fixed:* a backtick name inside an index of a hierarchical call
     (`` g[\a`b ].f(x) ``) still goes through `odin3_ast_print` while parsing and logs one
     unlocated `written as stored` warning; the parse succeeds and the round trip is equal
     (Task 5 re-review residual).
   - *Plan points (a)–(s) as agent defaults* (`docs/plans/2026-10-11-2C-verilog-parser-plan.md`):
     (a) one location type `odin3_vp_loc`, `TOK_` token prefix, the `%union`; (b) the parser
     input `odin3_vp_stream`; (c) the per-run read context `odin3_vl_run`; (d) `YYMAXDEPTH`
     through a `parser.c` static under `parse.lac full`; (e) Bison's `detailed` message with
     `syntax: ` for `syntax error, `; (f) the precedence block arrives with its rules; (g) node
     ranges start at the node's own first token, attributes attached by the enclosing rule; (h)
     one located error per run of bytes ≥ 0x80; (i) the rows rejected before or by 2C keep
     parsing variants, the rejecting forms are `tests/verilog/reject/` cases; (j) the reject
     driver runs in the case directory with the given paths; (k) `` `resetall `` leaves the
     keyword stack alone; (l) the 2D entry `odin3_elab_run` in `elab.h`, a stub until 2D; (m)
     `library_units_from` is the root index of the first library `UNIT`; (n) koios `lenet.v`
     runs last, the ≤ 5 s target covers `verilog/*.v`; (o) `odin3-parse-check` runs each file
     from its own directory through `odin3_read_verilog_files`; (p) `rows.txt` assigns each
     coverage row to its task; (q) `duplicate_module` located at the second definition's
     keyword; (r) the error cap counts errors only, the 21st becomes `too many errors;
     stopping`; (s) `vp_*` arity ≤ 5, enum-valued nonterminals through `vp_value`.
   - *Heavy runs:* the three sweep CTests are registered through `odin3_add_sweep_test()`, so
     `tools/heavy-run` alone gates (77 without `ODIN3_HEAVY_OK=1`), locks, caps and decides
     nesting; `parse-corpus` keeps no heavy logic (`--heavy` removed; Task 10 re-review R1/R2).

16. **Peter's rulings of 2026-10-12** (Peter: "Accept all recommendations other than 6").
    - **A stray `;` module item is accepted with a warning,** as Yosys 0.55 and Icarus accept
      it. This replaces 2C's rejection in #15's declared divergences.
    - **Confirmed agent defaults:**
      - 2E §20 #18: 2E gives Verilog's value where Yosys 0.55's `casez` mux grouping gives wrong
        logic (correctness over parity);
      - 2D: the top keeps its plain name under parameter overrides, as `chparam` does;
      - 2D: black boxes are ports-only `(* blackbox *)` MODULEs;
      - 2D divergence 26: project parameter numbers keep their sign.

17. **IR-8 amendment approved** (Peter, 2026-10-13: "IR-8 approved"). A black box may carry table
    width rules: the width of a port of a `(* blackbox *)` module with several parameter sets is
    a lookup of the cell's parameter values (docs/IR.md IR-8, IR-7b; `src/ir/celltype.h`). Merged
    with 2D-5 (#86).

18. **2D elaboration: what landed, the amendments, the evidence and the open items** (2D Tasks
    1–27, 2026-10-11 … 13; spec `docs/specs/2026-10-11-2D-elaboration-design.md` v4.26; plan
    `docs/plans/2026-10-11-2D-elaboration-plan.md`). Agent defaults throughout; Peter may override
    any of them.
    - **Landed:** PR 2D-1 (#54: IR changes, `write_rtlil`, ABI additions), 2D-2 (#56: constants),
      2D-3 (#79: hierarchy, parameters, specializations, binder), 2D-4 (#81: generate,
      declarations, REJECT/IGNORE), 2D-5 (#86: wires, expressions, drivers, memories, exact
      black-box widths), 2D-6 (#88: processes), 2D-7a (#90: `elab-check`, `elab-sweep`, coverage
      rows and goldens, the project-corpus frontend, the micro sweep), 2D-7b (this entry: the VTR
      sweep, `bench_elab`).
    - **The three IR changes, landed, for Peter's review** (agent defaults ruled in #13, confirmed
      in #14; IR-8's table width rules were approved in #17):
      - **A** — nine registry cells with Yosys simlib semantics: `$neg $xnor $reduce_xnor
        $reduce_bool $logic_not $logic_and $logic_or $shiftx $pow` (`src/ir/cells/`);
      - **B** — processes as `$proc` cells with real `I`/`O` ports and an opaque `BODY`
        (`ODIN3_VAL_PROC`, validated by `verify`), the `ODIN3_CT_PROCESS` type flag, the
        per-module process counter and `odin3_pass_def.processes` (default `REFUSE`; IR-19);
      - **C** — `odin3_port_spec` gains `has_range`, `msb`, `lsb`, `is_signed` (no ABI mirror).
    - **ABI v4 kept, unreleased** (ruling I1, plan point (c); spec v4.4 EL-47: the "one bump" text
      is gone). Every 2D addition is additive in 1D's sense. **Decision item:** the one bump of
      `ODIN3_ABI_VERSION` is made at the first release or at the Phase-2 close, whichever is first.
    - **Peter's rulings of 2026-10-12 (19:51), recorded:** (3) stage 1 specializes black-box
      definitions per parameter set, so each instance has exact port widths (landed with 2D-5:
      `elab1_bbox.c`, `elab2_bbox.c`); (4) Task 18's Yosys-matching widths are kept (comparison
      operands keep their own widths; a ternary's `$mux` is at the branch width and then extended);
      (5) a value on both declarations of a port stays an error (spec §17 #37); (6) the post-read
      check "net has sinks and no driver" stays a warning and "net has no pins" becomes info (a
      small follow-up; the 2D ledger tracks it).
    - **Spec amendments and errata** (`docs/specs/2026-10-11-2D-elaboration-design.md`, header and
      change notes):
      - the two v4 amendments from the plan review, both following Yosys 0.55: EL-39's write
        temporaries start each block as `addr = x`, `data = x`, `en = 0` (not `en = 0` alone: a
        path that never writes left `addr`/`data` unassigned, a combinational loop after `proc`);
        EL-30 lowers `~&a`/`~|a` to `$logic_not` of the reduction (not `$not`);
      - **erratum of plan point (h)**: a sync on an `OUT` position is legal for edge and level
        syncs (§8.1 is normative and carries re-review N1); §14.1's "a sync signal that is an OUT
        position" among the rejected bodies is a stale v2 phrase;
      - **spec v4.4 errata** (the 2D-1 final review, I1 and M1): §13 gains an implementation note
        with the exact `write_rtlil` text 2D-1 ships (`src/backends/rtlil/writer.h`; eight places
        where it differs from §13's bullets, and the `attribute \top 1` line for the design's top),
        which 2E-1 and 2F-1 read; EL-47 says the ABI stays at the unreleased v4 and that IR
        change C has no ABI mirror;
      - the divergences from Yosys/Parmys and from IEEE 1364-2005 are spec §17 (75 rows, #1–#75),
        by reference; the open one for Peter is EL-8 (the first `defparam` wins, as in Yosys;
        IEEE 1364-2005 §12.2 makes the last one win).
    - **Parmys exceptions added to #4** (micros Parmys itself rejects; for these 2D rejects too, the
      reasons are in `tests/elab/micro_expect_fail.txt`, 79 entries): the 20 micros 2C parses and
      2D's elaboration rejects (a name declared twice, `'x' has two drivers`, a recursive call that
      cannot be inlined, a procedural `assign`, a non-constant index into a multi-dimensional
      register array, …); the 8 `*_failure` micros; the 14 SystemVerilog named-argument micros; the
      3 splicing micros (#11); the 13 includer-macro fragments (#12); the 12 illegal-Verilog-2005
      micros of #15. On top of #15's list.
    - **Exit-bar exceptions that need Peter** (the 2D-7b triage of the micro sweep, which has
      539 files: 460 pass, 79 listed, 0 unexpected). These 9 have Parmys goldens, so they are not
      Parmys exceptions; each is listed in `tests/elab/micro_expect_fail.txt`:
      - (Superseded by #20: T6, D3 and T9 are now accepted with warnings; the adder gap stays an
        explained error.) 7 are rejected on purpose, soundness first (Yosys accepts them): `always` without an event
        control (T6: `keywords/task_endtask/{inside_port,outside_port,task_call_function,
        task_call_task}`), a tri-state driver on an internal net (D3: `syntax/multi_assignment`), a
        bit assigned by both `=` and `<=` (T9: `full/matmul`, `syntax/multi_clock_reader_writer`);
      - 2 are gaps for 2D-7b: positional width `#(W)` and bus connections on the hard `adder`
        (`micro/adder_hard_block`, `micro/hard_adder_cin_propagation`; Parmys builds a W-bit adder
        chain from the 1-bit hard block).
    - **Plan points (a)–(bb) as agent defaults** (plan section "Spec points made exact here";
      Task 27 logs them here, each reversible):
      - (a) `odin3_elab_run(ctx, design, parsed, opts)` takes the options; the parsed store is
        handed to the design first and dropped by the entry's step 3, never destroyed by it;
      - (b) the ABI form is `odin3_ast_elaborate(design, ast, opts)` on a held, sealed
        `ELABORATED` store; (c) ABI v4 stays (above); (d) IR change C has no ABI mirror;
      - (e) `ODIN3_VAL_PROC` is one 8-byte-aligned blob in canonical case/switch/action order,
        written field by field into zeroed memory (so copy, compare, `compact` and tombstones need
        nothing); `verify` checks the layout without allocating;
      - (f) the nine cells, then `$proc`, are appended after `$sop` (built-in IDs 0–52 do not
        move; the count is 63); (g) `$proc`: ports `I`/`O`, defaults `I_WIDTH` 0, `O_WIDTH` 1, an
        empty `BODY` that `verify` rejects, flag `ODIN3_CT_PROCESS` = 32, and only the built-in
        may have a `PROC` parameter;
      - (h) a sync on an `OUT` position is legal (erratum above); (i) the `processes` check runs
        in `run_def` before `odin3_pass_run_begin`, so a refused pass opens no run;
      - (j) the header split (`cval.h`, `ceval.h`, `elab.h`, `elab_internal.h`, `pg.h`,
        `elab_test.h`, `elab1.h`); (k) the run-level iteration cap crosses the seam without a
        field (stage 2 starts at the number of indexed `GEN_BLOCK`s);
      - (l) not-yet-built constructs were a located error `elaboration is not available yet (2D)`
        until the tables emptied (`elab_no_pending` asserts they are empty);
      - (m) `--x-report` and the defined-output miter stay in 2H (below);
      - (n) every sweep runs through `tools/heavy-run` in the 6G slice (the 9G slice for
        `sweep-xl`), every child through `memcap.py`; `--jobs J` is refused when `J × cap` exceeds
        60% of `MemTotal` or the slice; builds go through `tools/build-locked`; the 64 VTR and
        Koios files are one set, smallest first, `lenet.v` last;
      - (o) a micro's `--top` is the first `.model` of its Parmys golden (`tools/elab-check/
        micro-top`); (p) Yosys 0.55 is found beside VTR's build and must say `Yosys 0.55`; 0.69 is
        the nightly drift check only; (q) name collisions take `$2`, `$3`, … in specialization
        order, and a name over 128 bytes is hashed before the check; (r) a `$proc` prints as
        `$proc$<file>:<line>$<ID>`; (s) one sigspec spelling, `odin3_proc_sigspec`;
      - (t) `tests/bench/bench_elab.c`, run per file by `elab-sweep --bench`; (u) the test files
        `test_elab_stage1_*.c` and `test_elab_stage2_*.c`; (v) a construct elaborated several times
        warns once; (w) the texts §9 leaves open; (x) `processes` is `OK` for `read_project`,
        `dump_project`, `preprocess` and `read_verilog`; (y) a relative `$readmem` path resolves
        against the directory of the call's file; (z) the text and count fixes (54 hand-written
        rows; `$clog2`'s error text); (aa) the cap texts; (bb) the spelling of `NUMBER`s stage 1
        creates (32-bit decimal integers for counts and bounds, an unsized parameter keeps its
        base, everything else binary).
    - **Evidence tools** (Tasks 25–27): `elab-check` (125 coverage rows, 20 with `--nolatches`, 125
      `--stage1`); `elab-equiv` (the Yosys 0.55 bridge, exit 3 = oracle crash); `elab-sweep` (micro
      sweep `verilog_micro_elab` and `elab_equiv_micro`, the VTR sweep `verilog_vtr_elab` and
      `elab_equiv_vtr` / `elab_equiv_vtr_xl`, `--bench`); `bench-report` (the §15 targets, the
      smoke). The expected-failure lists are `tests/elab/{micro_expect_fail,micro_equiv_expect_fail,
      vtr_expect_fail,vtr_equiv_expect_fail}.txt`. A memory-cap kill, a signal and a timeout are
      never listed as expected (Task 26's rule), so the plan's candidate `out of memory (cap 8G)`
      entry for `koios/lenet.v` is the `xl` run class in `vtr_equiv_expect_fail.txt` instead: the
      `elab_equiv_vtr_xl` run reports the Yosys side's peak RSS or its cap outcome.
      **2H amendment (agent default, Peter may override):** the rule stands for odin3 and Yosys
      side A (a cap kill, signal or timeout there is hard, never listed). Two outcomes that are the
      oracle's or the checker's size, not odin3's, are `unproven` (`elab-equiv` exit 4) and listable
      with a needle: Yosys side B killed by its cap (`unproven: Yosys side B exceeded its memory
      cap (4G)`) and `elab-equiv --budget` running out (`elab-sweep --equiv-budget 1800`: the
      Yosys and equiv-check wall time of one file, odin3's excluded). `micro_equiv_expect_fail.txt`
      lists `full/stereovision0`, `large/LU32PEEng`, `LU64PEEng` (side B), `large/arm_core`
      (budget) and `cf_fir_3_8_8`, `stereovision2`, `bgm`, `cf_fft_1024_16` (ABC dsec 600 s) with
      the measured evidence; `large/mkDelayWorker32B` runs at a 6G cap (`--cap-for`) and ends at
      the `dual_port_ram` pairing limit of its siblings.
      A host OOM-kill (or manual kill) of side B is indistinguishable from a cap kill: SIGKILL is
      the only signal `elab-equiv` sees, so either reads `exceeded its memory cap`.
    - **Heavy runs** (controller, Release build, lanes idle; `ODIN3_HEAVY_OK=1`):
      ```
      tools/build-locked --preset release -j6
      ODIN3_HEAVY_OK=1 ctest --preset sweep -R '^(verilog_vtr_elab|bench_elab_smoke)$'
      ODIN3_HEAVY_OK=1 ctest --preset sweep -R '^elab_equiv_vtr$'
      ODIN3_HEAVY_OK=1 ODIN3_HEAVY_XL_OK=1 ctest --preset sweep-xl -R '^elab_equiv_vtr_xl$'  # no agent running
      VTR=$VTR_ROOT/vtr_flow/benchmarks/verilog
      tools/heavy-run tools/elab-check/elab-sweep --odin3 build/release/odin3 \
        --bench build/release/bench_elab --bench-report elab --techlib lib/vtr.o3lib \
        --jobs 1 --mem-cap 3G --cap-for koios/lenet.v=5G --expect-fail tests/elab/vtr_expect_fail.txt \
        --root $VTR $VTR/*.v $VTR/koios/*.v
      ```
    - **Results.** Smoke subset (Task 27, Release, dev machine: the five smallest designs and
      `mcml.v`; the three `koios/*_include.v` files are the listed `define-only fragments):
      ```
      bench: file=mcml.v bytes=650837 parse_ms=19.197 stage1_ms=25.487 bind_ms=3.580 stage2_ms=250.370 elab_ms=279.437 parsed_nodes=78229 parsed_bytes=4235356 elab_nodes=67039 elab_bytes=3369948 modules=36 specs=0 cells=2449 procs=121 mems=0 nodes=3372 nets=335520 wires=6057 pins=592190 est_pins_B=21555716 est_nets_B=16104960 est_nodes_B=161856 est_wires_B=387648 est_bodies_B=2928856 est_elab_B=3369948 est_total_B=44508984 est_stage1_B=7605304 ir_B=42099248 rss_kib=59552
      elab-sweep: 9 files, 6 passed, 3 expected failures, 0 unexpected failures, 0 listed but passing, 0 oracle crashes, 0 skipped, 0 stale entries, 0.3 s
      bench targets (spec 15):
        ok     mcml.v stage 1 + stage 2: 0.279 s (target <= 0.5 s)
        ok     mcml.v peak RSS: 60.981 MB (target <= 200 MB)
      ```
      The full 64-file bench output (every line, `lenet.v` last, with the §15 target block), the
      `verilog_vtr_elab` run log and the `elab_equiv_vtr` / `elab_equiv_vtr_xl` summaries replace
      this block when the controller has run them; a missed target is investigated with its
      estimate terms and reported, never relaxed (2A's rule).
    - **2H backlog item** (plan point (m); spec §14.6 items 3–5, owned by 2H): `equiv-check
      --x-report` (when two netlists are not equivalent, recompute with every primary output and
      latch input whose sequential fan-in cone reaches `unconn` or an x constant removed, and report
      the removed outputs by name), its latch handling and its exit-code contract, and the
      defined-output miter (x as a free input `u`; prove that Odin III's output agrees with Parmys
      wherever it is defined). Their input is `elab-equiv --a-only --keep DIR`, which writes side
      A's netlist as `a.blif` with Parmys's `write_blif -gates -undef + unconn` convention for
      comparison with the goldens. The 2H table has four outcomes per design: identical,
      equivalent, equivalent outside x-cones, differs (#13's exit-bar ruling).

19. **IR-6 unchanged** (Peter, 2026-10-13: "do recommended"). `opt` does not compact; scripts run
    `compact` after `opt` (the Phase 2 script is `opt; compact`). This withdraws the 2F Task 13
    IR-6 amendment (spec 2F §17 #39). The sub-passes never compact either.

20. **Peter 2026-10-13: accept T6/D3/T9 with warnings** (2D-7b carry; Peter: "accept all 3").
    Parmys accepts the seven micros that use them, so 2D accepts the constructs with a located
    warning each (spec §8.2, §8.9, §9.2, §17 #76-#78):
    - **T6**: an `always` with no event control is `always @*` ("always without an event control
      treated as always @*"). Micros `keywords/task_endtask/{inside_port,outside_port,
      task_call_function,task_call_task}` are `elab-equiv` equivalent to Yosys 0.55.
    - **T9**: `=` and `<=` on one bit follow Yosys (every assignment is an action, the last one on
      a path wins) and warn, naming the variable; on one control path the warning also says that
      simulation and synthesis may differ. `full/matmul` is equivalent; `syntax/
      multi_clock_reader_writer` is equivalent under a merged-clock abstraction (`equiv-check` is
      single-clock). T10 (two blocks, one bit) stays an error.
    - **D3**: an internal tri-state net becomes mux logic as `tribuf -logic` does (`$pmux` of the
      data by the enables, x when none is enabled); top-level output and inout tri-states keep
      their handling. **Done** (spec §6.7, §9.2, §17 #79): micro `syntax/multi_assignment` and
      `tests/elab/accept/D3_{single_driver,bus_one_hot,below_top}.v` are `elab-equiv --yosys-tribuf`
      equivalent. Yosys 0.55's plain flow shorts the four drivers' data, so the equivalence is
      judged against `tribuf -logic` after `proc; flatten`, not the plain Parmys flow. Several enables
      at once: undefined, as a `$pmux`; a mixed tri-state and ordinary net stays an error.
    - **Adder**: `adder #(W)` on the hard `adder` is an error unless `vtr_primitives.v` is read with
      `read_verilog --lib` (the library-unit read below, spec §3.1a, §17 #78): the library cell is the 1-bit hard block;
      Parmys's n-bit adder is a soft module of that file. With it the micros elaborate and are
      `elab-equiv` equivalent; matching Parmys's chain of 1-bit hard adders is 3E's (§17 #80).
    - **The library-unit read** (agent default, spec §3.1a, §17 #78, #80; the S-P splice of
      2F-5 and the two `adder #(W)` micros). `read_verilog --lib [--nolatches] [--nomem2reg] FILE...`
      keeps a read's modules as library units for later reads (not Yosys `-lib`: no black boxes).
      They are never top candidates, are dropped without a word when unused, and each is elaborated
      where a design instantiates it under the flags of the read that supplied it (the design read's
      flags do not reach it). A unit named like a hard cell (`adder`) replaces the cell for an
      instance only when the instance's `#()` does not fit the cell, with a warning; otherwise the
      cell stays, with an info line. `tools/opt-check/sp-splice` (2F-5, not on this branch) reads
      `vtr_primitives.v` with `--lib --nomem2reg` in place of its plain read; `elab-equiv` and
      `elab-sweep` take `--lib FILE`. The loose ends of the 2D-7b review (M5) are closed: the "3E"
      wording, the exemption of a library unit from the cell-name check, and the structural gap to the
        Parmys goldens (§17 #80).

21. **2E `proc`: what landed, the agent defaults and the bench** (2E Task 13, 2026-10-13; spec §19,
    §20, §23; plan points (a)–(u)). Landed as PRs 2E-1 to 2E-4 (the last as 2E-4b). Peter reviews the IR items below;
    everything else is an agent default (reversible, logged here per the plan).
    - **`proc`** (`src/passes/proc/`, docs/PASSES.md): Yosys 0.55's `proc -norom` on 2D's `$proc`
      bodies, mode P (it runs on the output of `opt_expr; opt_clean; opt -nodffe -nosdff`, or
      without it). Step F is 2F's `opt_expr -keepdc` (`proc_step_f`), absent under `--noopt` and
      `--stop-after`.
    - **Landed, for Peter's review:** IR change D (spec §13; docs/IR.md IR-8): the eleven word
      flip-flop and latch cells `$adffe $sdffe $sdffce $aldff $aldffe $dffsr $dffsre $dlatch
      $adlatch $dlatchsr $sr`, with simlib's ports and parameters and no simulate hooks, written
      as generic instances (point (b)); and `odin3_attr_remove` (IR-10; point (e)), which `ffinit`
      needs for "an attribute left all x is removed". Also `odin3_design_bytes_reserved`
      (read-only, IR-8) and the shared byte budget (`src/passes/budget.[ch]`: 60% of physical
      memory, one check for 2E and 2F).
    - **Agent defaults, points (a)–(u)** (plan "Spec points made exact here"; none needed Peter):
      (a) `yosys-phase` in Task 2; (b) IR change D needs no writer edit; (c) `src/passes/opt/opt.h`
      from 2E-1; (d) the budget's message is built apart from the check; (e) `odin3_attr_remove`
      and `odin3_ffinit_remove`'s `defined_on`; (f) step F's call site (below); (g) the chunk
      check (below); (h) pending phases kept `main` green until Task 9 (gone); (i) `pw.attrs`,
      `pw.lowered`, `odin3_proc_cell.{pw,output,attrs_from}`; (j) statistics; (k) diagnostics
      without a location; (l) the rows' goldens are `proc --noopt`; (m) five error rows; (n) E4 is a
      step-A check; (o) `tools/proc-check/int_params.txt`; (p) heavy runs (`tools/heavy-run`,
      `odin3_add_sweep_test`, 6G and 9G slices, builds through `tools/build-locked`); (q) the bisect
      lists; (r) `elab-equiv --pass proc` in Task 11; (s) `$tmp` names in `proc_build`; (t)
      `proc-diff` imports `memcap` lazily; (u) the net map keeps the lower net ID when both
      primary wires are port wires.
    - **Divergences** from Yosys 0.55 and Parmys: spec §20, #1 to #19, by reference (the ones with
      a correctness argument are #17 (an all-`-` item of a `parallel_case` group: lowered, where
      Yosys asserts), #18 (Yosys 0.55 keeps a mux open across a group boundary in `casez` and gives
      wrong logic, proved by a sat counterexample and confirmed by Icarus: 2E gives Verilog's value;
      correctness over parity) and #19 (a mux cycle in the latch walk reads as no feedback, where
      Yosys never terminates)). §9 is amended: a register is built from the last sync's first
      assignment, as Yosys's `find_any_lvalue` does.
    - **Point (f), step F:** wired by 2E itself, in PR 2E-3 (`proc_step_f` calls
      `odin3_opt_expr_run` with `keepdc`, and `test_step_f_folds` pins it), because 2F-2's
      `opt_expr` had merged first. The info line reports `opt_expr: K changes`.
    - **Point (g), the chunk check:** 2D-6's process generator emits one action per chunk of each
      left side and groups case temporaries by the same chunks (2D spec v4.2), and constant `I` pins
      follow net-ID order. Outcome: no mismatch found. The rows' lowering (48
      `proc_rows`, 43 against Yosys 0.55, both latch modes) is identical to Yosys's, including
      `case_default`'s single 8-bit `$pmux`; decode-to-encode identity was not run separately on
      2D's real bodies (the identity tests use hand-built bodies), and plan point (g)'s third
      bullet (every body position referenced by a span, no empty span) was not checked. (The 2E-3
      PR description did not record the outcome; reconstructed from Task 11's evidence,
      2026-10-13.)
    - **`opt` is still 2F-5's stub**, so the sweeps and the bench run the pre-`proc` passes
      without it: `proc-diff --trio auto` falls back to `--no-trio`, and `bench_proc` reports
      `trio=partial` (`opt_expr`, `opt_clean`). The numbers below are to be repeated with the full
      trio once `opt` lands; the cell counts and the input differ by `opt`'s changes.
    - **For 2H's exceptions list** (spec §4, §18.6; `tests/proc/micro_expect_fail.txt`). Five
      micros 2E rejects, each with its error kind: `keywords/always/always_posedge_negedge.v`,
      `syntax/bm_function_1.v`, `syntax/bm_function_2.v` and `large/matrix_multiplication.v` are
      real two-clock blocks (E1a, `two clocks`: Yosys 0.55 fails on them too), and
      `large/paj_top_hierarchy_no_mem.v` has a non-constant reset value (E2: Yosys fails too). Of
      the nine micros Parmys rejects with `Multiple edge sensitive events found for this signal!`,
      three more lower in 2E and in plain Yosys 0.55 `proc`, and `proc_diff_micro_lowers` asserts
      they are identical in both input modes: `large/paj_framebuftop_hierarchy_no_mem.v`,
      `large/paj_raygentop_hierarchy_no_mem.v` and `syntax/diffeq_f_systemC.v` (their resets are
      found as nets, so `opt_expr`'s connections do not hide them, §4). The ninth,
      `large/paj_boundtop_hierarchy_no_mem.v`, is a 2D reject (`'debugcount01' has two drivers`, a
      typo in the source); its typo-fixed copy `tests/proc/paj_boundtop_typo_fixed.v` stands in for
      it and lowers identically. All nine stay Parmys-rejected exceptions of the exit bar (#4).
    - **The bench** (`tests/bench/bench_proc.c`, Release only: `bench_guard.h`, 2D's, refuses a
      Debug or sanitized build with exit 2; one warm-up pass, then the measured one). `bench_proc
      --one F [--techlib T] [--no-trio] [--noopt]` prints one `key=value` line: bytes; the untimed
      `read_verilog --nolatches` and trio; the time of steps A, B, C, D, E and F, `noopt_ms` = A to E
      (§19's targets are `proc --noopt`), the whole `proc_ms`, the byte-budget share
      (`budget_ms`, `budget_calls`: `odin3_design_bytes_reserved` is O(modules) and the budget is
      checked at every process boundary, O(modules x processes), 2E Task 3 review finding 6);
      processes, the cells created per kind and nets, nodes and pins created; the §19 estimate
      terms (mux pins x 28 x 1.3, nets x 48, nodes x 48, the working forms' peak) and `ir_B`; peak
      RSS (`getrusage`). The steps are timed through the `on_step` hook of `proc_test.h` (added in
      Task 13, with a unit test). The set is the 64 files as one set by size, `lenet.v` included
      (Odin III's side fits the 6G slice), through `elab-sweep --bench` (which runs `B --one F`
      only, passes no top and defaults to 4G, hence `--mem-cap 6G`); `tests/proc/
      bench_expect_fail.txt` lists the three macro-only Koios include files (no module), the two
      `bwave_like.fixed.*` files (ambiguous top: `bench_proc` has no `--top` and `elab-sweep
      --bench` passes none, a 2D follow-up) (`eltwise_layer.v` and `dnnweaver.v` were listed too, as
      rejected until 2D-7b: eltwise_layer's line went with T9's acceptance, dnnweaver's with the
      2D-7b carry fix, a string parameter widened by `?:` with a NUL byte, an unlocated
      `ODIN3_ERR_INVALID_ARG`, and a `$readmemh` under a constant `if`, §17 #67), so 28 Koios
      files are timed.
      `tools/proc-check/bench-proc-report report --expect-fail tests/proc/bench_expect_fail.txt
      LOG` prints §19's targets from the sweep's output and exits 1 on a MISSED one, with the
      estimate terms and step times of the files it names. A file or set missing from the log, or a
      row short of a column, is MISSED unless the expect file lists it: partial data never passes.
      The slice margin is nil (`--jobs 2 --mem-cap 3G` is the 6G slice, as 2D's). `noopt_ms` equals `proc --noopt` run alone to within 0.1% (mcml.v, diffeq1).
      `proc_diff_micro_lowers --also-no-trio` runs one mode only, vacuous until 2F-5.
      The full bench, from a Release build, on an idle machine (the controller):
      `ODIN3_HEAVY_OK=1 tools/heavy-run tools/elab-check/elab-sweep --odin3 build/release/odin3
      --bench build/release/bench_proc --techlib lib/vtr.o3lib --mem-cap 6G --jobs 1 --expect-fail
      tests/proc/bench_expect_fail.txt --root $VTR_ROOT/vtr_flow/benchmarks/verilog
      $VTR_ROOT/vtr_flow/benchmarks/verilog/*.v $VTR_ROOT/vtr_flow/benchmarks/verilog/koios/*.v
      | tee LOG` under `set -o pipefail` (elab-sweep has no `--log`; without it the sweep's exit
      code is lost to `tee`), and then `tools/proc-check/bench-proc-report report --expect-fail
      tests/proc/bench_expect_fail.txt LOG` .
    - **Results (§19 targets): NOT YET FILLED; the controller records the full run here, verbatim (the
      sweeps' summaries and the tie count too). `lenet.v` time and RSS are unmeasured; only a
      3G-capped smoke on seven small files exists.**
      mcml.v `proc --noopt` <= 0.2 s; lenet.v `proc --noopt` <= 5 s; lenet.v through the Phase 2
      script to `proc`, peak RSS <= 3.0 GB; the 31 VTR files <= 0.5 s in total; the 33 Koios files
      <= 8 s in total. A miss is investigated with the printed terms and reported, never relaxed.
    - **Sweeps' summaries and `sweep-xl` (lenet.v, Yosys side, 9G slice): not yet run (needs an
      idle machine).**

22. **IR change journal approved** (Peter, 2026-10-13, option 1a; IR-18a in `docs/IR.md`; spec 2F
    §19). `opt_dff` and `opt_clean` skip work that the IR's change journal shows cannot have
    changed: after its first sweep of a module in an `opt` invocation, `opt_dff` visits only the
    flip-flops whose cone the journal touched; `opt_clean` skips a module's sweep when its last
    sweep changed nothing and the journal's mutation count, the design's keep-module flags and
    `-purge` are all as then. Conditions Peter set: every new allocation (the journal's growth,
    the dirty marks, the walk arrays, the keep-flag copy) is reached by an OOM sweep and falls
    back to a full sweep with the IR valid and the output byte-identical (`test_opt_oom_fallback`);
    the journal's bytes count in `opt`'s budget, its 16 MB cap kept (`test_budget_journal`); Debug
    builds sweep the modules `opt_clean` would skip and abort if one changes anything, and visit
    every flip-flop `opt_dff` skipped, each at its own position in the sweep, likewise (a skipped
    flip-flop that the sweep itself dirtied is covered). Nothing outside `opt` may depend on the journal.
    The output is byte-identical to the full sweeps (82 files in 41 designs; `rescan-compare`; the mcml.v `opt` time of 0.87 s is
    `bench_opt`'s median on an idle machine, 1.0 s under load).

23. **2F `opt`: what landed, the agent defaults, the evidence and the bench** (2F Task 19,
    2026-10-13; spec §17, §19, §20, §23; plan points (a)-(s)). Landed as PRs 2F-1 to 2F-5, the
    2F-perf PR (#93, #22) and 2F-6 (this branch, `feat/2F-evidence`). Peter reviews the items under
    "For Peter" below; everything else is an agent default (reversible, logged here per the plan).
    - **`opt`** (`src/passes/opt/`, docs/PASSES.md, eight rows, all `processes = OK`, mode P: every
      `$proc` is opaque): Yosys 0.55's `opt` family as a fixed-point loop (`opt_expr`, `opt_merge`,
      `opt_muxtree`, `opt_reduce`, `opt_share`, `opt_dff`, `opt_clean`), capped at 10,000
      iterations. `opt` never compacts (#19). DESIGN §6 step 4 now says so; the 2D spec's EL-33
      sentence on structured body accessors reads "deferred (2E §24 item 8)".
    - **Divergences from Yosys 0.55** are spec 2F §17 (rows 1-56), by reference, each with its
      reason and the file or seed that shows it. The ones the evidence added in 2F-6: #53
      (`bm_stmt_compare_padding.v`, an `$sdff` merge Odin III misses), #54 (`bm_sfifo_rtl.v`, a
      `mux_invert` and a `const.$neg` Yosys fires), #55 (mixed-sign cells, below; superseded by #57 / PHASE2 #25) and #56 (`--opt`
      seeds 100 and 153, a `$sdff` on one side only). Sound fixes of Yosys miscompiles (#47 and
      others, bitwise-only) are listed there too.
    - **Agent defaults, plan points (a)-(s)** (`docs/plans/2026-10-11-2F-opt-plan.md`, "Spec
      points made exact here"): (a) entry-point names, `odin3_opt_expr_run` fixed by 2E-1;
      (b) one `odin3_opt_counts` aggregate; (c) options, Yosys spellings plus `--` spellings,
      `-share_all` and `-sat` refused; (d) the budget message names the running pass; (e) pending
      sub-passes were stubs (all removed by 2F-5); (f) per-rule goldens from Verilog moved to
      2F-6 Task 16 (`opt_rules`); (g) `opt-rules` uses `read_verilog --nolatches; proc --noopt` as
      its input; (h) the compact point, **superseded by #19**; (i) the Phase 2 script
      `tools/opt-check/phase2.script` and S-P; (j) rule counters under Yosys's cover names; (k)
      `opt_dff` action counters; (l) constbits sees no tie made in the same call (mcml.v: 31
      iterations); (m) the in-process 1E check (`--opt-simcheck`) judges only what it can
      simulate; (n) Yosys-side lenet.v runs are `sweep-xl`; (o) `order-probe.sh` is evidence only;
      (p) CSE survivors are rebuilt in place; (q) the loop-cap warning names a module; (r)
      `opt_muxtree`'s budget counts `eval_mux` calls; (s) the init-conflict warning quotes the net.
    - **Step F** (2E point (f), `opt_expr -keepdc` inside `proc`) was wired by 2E itself in PR
      2E-3 (`proc_step_f`), as `odin3_opt_expr_run` came from 2E-1; 2F-2 made the function real.
    - **Performance** (spec §19; IR change journal, #22): `opt_expr` is incremental after its first
      iteration; `opt_dff` and `opt_clean` skip what the journal shows unchanged. Byte-identical to
      the full sweeps (82 files, 41 designs, `rescan-compare`).
    - **Evidence tools** (all Yosys 0.55 oracle; 0.69 only in the nightly legs): `opt_rules_yosys`
      (141 rule files; 138 match Yosys's cover point or log line and cell histogram, 3 are
      documented §17 rows), `opt_equiv_rules` and `opt-equiv` (per-row refinement, one direction:
      the after-state refines the before-state; it is Yosys `equiv_make`/`equiv_simple`/
      `equiv_induct` run by `opt_equiv.Checker` on both sides flattened, not `equiv-check`, and
      not a mutual check; clock-aware: 138 proved, 3 documented skips, 0 failed; 12 mutants fail),
      `opt-diff` / `opt_diff_micro` (Phase 2 script and `opt -full` against Yosys; on 487 micros
      up to 5.9 KB 367 ok, 81 expected failures, 38 designs listed by class in
      `tests/opt/diff_expect.txt`: 28 proved, 2 proved for the Phase 2 script only, 8 structural
      only (no tool verdict; diffeq_paj_convert did not finish); no opt soundness bug found, but
      those 8 and the 2 partial ones are not proved), `opt_simcheck_rules`,
      `opt_micro` (490 micros: 409 ok, 81 xfail, 0 unexpected, at most 3 iterations),
      `opt_gen_vs_yosys` (the `--opt` generator, 200 seeds, 0 failures, structural cases listed in
      `tests/opt/gen_structural_expect.txt`), `fuzz_opt` (CI; it found two real bugs, mixed-sign
      `identwrt` and `fine.addsub`, fixed under the both-signed rule; #55 is superseded by §17 #57 / PHASE2 #25). These are the implementers'
      smoke numbers; the sweeps' own summaries are in the Results block, to be filled.
      **Exit codes of the opt tools (one convention):** 0 pass, 1 a real negative result, 2 usage
      or tool error, 3 hard (a crash, signal, memory-cap kill or timeout of a child; `opt-equiv`,
      `opt-diff`, `opt-rules`, `opt-run`, `opt-gen-diff`), which elab-sweep and opt-nightly treat
      as hard: no expect-fail line absorbs it. `opt-equiv --min-changed N` fails a run in which
      fewer than N sub-pass calls changed something (CTest `opt_equiv_rules` passes 1; 19 sources
      whose rule Phase 2 does not run are listed in `rules_equiv_expect_fail.txt`).
    - **The bench** (`tests/bench/bench_opt.c`, Release only: `bench_guard.h` refuses a Debug or
      sanitized build with exit 2). `bench_opt --one F [--techlib T]` prints one `key=value` line:
      cells, nets and pins after the read (0), after `proc` (p) and after the script (1); for the
      script's opt passes and for `opt -full` (prefix `full_`) the iterations, `t_total`, the time
      per sub-pass and `t_setup`, the rewrites per sub-pass and per rule family, the dead objects
      at the peak, `t_compact`, and per-phase RSS (`VmHWM`, `rss_src=hwm`). The target column for
      lenet.v is `rss_opt_growth_pct`. `--techlib` is new in Task 19 (the sweep passes it; before,
      only `ODIN3_BENCH_TECHLIB` worked). `tools/opt-check/bench-opt-report report --expect-fail
      tests/opt/bench_expect_fail.txt LOG` prints §19's targets from the sweep's output and exits 1
      on a MISSED one, with the times and memory columns of the files it names; a file or set the
      run lacks, or a row short of a column, is MISSED unless the expect file lists it. The
      expect file is the same five files as `tests/proc/bench_expect_fail.txt` (three macro-only
      Koios includes, the two ambiguous-top `bwave_like.fixed.*`; `bench_opt` has no `--top`).
      CTest `bench_opt_smoke` (label `sweep`, Release) runs `--one` on two micros and checks every
      column.
      The full bench, from a Release build, on an idle machine (the controller):
      `ODIN3_HEAVY_OK=1 tools/heavy-run tools/elab-check/elab-sweep --odin3 build/release/odin3
      --bench build/release/bench_opt --techlib lib/vtr.o3lib --mem-cap 6G --jobs 1 --expect-fail
      tests/opt/bench_expect_fail.txt --root $VTR_ROOT/vtr_flow/benchmarks/verilog
      $VTR_ROOT/vtr_flow/benchmarks/verilog/*.v $VTR_ROOT/vtr_flow/benchmarks/verilog/koios/*.v
      | tee LOG` under `set -o pipefail`, then `tools/opt-check/bench-opt-report report
      --expect-fail tests/opt/bench_expect_fail.txt LOG`.
      Smoke (Release, 2026-10-13, one at a time through `heavy-run`, 4G cap; the host was shared
      with another lane's equiv-check, so times are an upper bound): diffeq1.v opt 0.002 s;
      ch_intrinsics.v 0.002 s; sha.v 0.024 s, RSS growth 9.8%; mcml.v 0.753 s (target <= 1.0 s,
      32 iterations in all, t_expr 0.107, t_merge 0.183,
      t_muxtree 0.096, t_reduce 0.049, t_dff 0.181, t_clean 0.138, t_setup 0.079) and
      `opt -full` 0.102 s (target <= 0.3 s). mcml.v's RSS growth over the post-proc IR is 28.9%
      (75,136 to 96,820 KiB; 9,796 dead nodes and 313,599 dead nets at the peak). The 25% target
      is for lenet.v, but mcml.v is over it: if lenet.v slips, the escalation (a compact per `opt`
      iteration) is an IR-6 amendment and goes to Peter, never a relaxed target.
    - **Results (§19 targets): NOT YET FILLED; the controller records the full run here, verbatim
      (the sweeps' summaries and `opt_order_probe`'s table too). `lenet.v` time and RSS are
      unmeasured; only the four-file smoke above exists.**
      mcml.v Phase 2 opt passes <= 1.0 s; mcml.v `opt -full` <= 0.3 s; lenet.v Phase 2 opt passes
      <= 15 s; lenet.v RSS growth <= 25%; the 31 VTR files <= 3 s in total. A miss is investigated
      with the printed columns and reported, never relaxed.
    - **Sweeps' summaries and Task 20's nightly legs (`tools/opt-check/opt-nightly`;
      `opt_equiv_vtr`, `opt_diff_vtr`, `opt_equiv_vtr_xl`, `opt_diff_vtr_xl`): not yet run in full;
      the controller runs them by hand (Peter runs no nightly) and records the summary lines
      here.** Each file runs `opt-equiv` and `opt-diff` with all five scripts (spec 20.3), one tool
      at a time under memcap, smallest first (`mcml.v` last). After `tools/build-locked --preset
      release -j6`, with no other heavy job and no agent running a build:
      - `ODIN3_HEAVY_OK=1 ctest --preset sweep -R '^opt_(equiv|diff)_vtr$'` (31 files, one tool
        child at a time, `--mem-cap 3G`); or by hand:
        `ODIN3_HEAVY_OK=1 tools/heavy-run tools/opt-check/opt-nightly --odin3 build/release/odin3
        --yosys $VTR_ROOT/build/bin/yosys --techlib lib/vtr.o3lib --set vtr31 --mem-cap 3G
        [--leg equiv|diff] [--keep DIR]` (add `--cap-for mcml.v=5G` if mcml.v meets its cap).
      - lenet.v's Yosys side, an idle machine only: `ODIN3_HEAVY_OK=1 ODIN3_HEAVY_XL_OK=1 ctest
        --preset sweep-xl -R '^opt_(equiv|diff)_vtr_xl$'`, i.e. `--set all64 --xl --mem-cap 8G`
        (`tests/opt/vtr_nightly_xl.txt` lists `koios/lenet.v xl`; `--cap-for koios/lenet.v=8G` is
        the same cap spelled per file).
      - Smoke only so far (Task 20): the 3 smallest VTR files (`single_wire.v`, `single_ff.v`,
        `and_latch.v`), 18 runs, 18 ok, 0 unexpected, 3G cap. The five-script `opt-diff` of the
        31 files has no `tests/opt/diff_expect.txt` lines yet: the full run adds them (or fails).
    - **Hand-off to 2H:** S-P (`tools/opt-check/sp-splice`, Odin III against Yosys through the
      Phase 2 script) is the exit table's `opt` row; S-D is information only. lenet.v's S-P needs
      about 7 GB (Yosys's side), so 2H runs it only as a `sweep-xl` job. The Odin III half reads
      `vtr_primitives.v` as a library unit with per-read flags (`--nomem2reg`); 2D-7b's
      `read_verilog --lib` is that mode, and 2H checks it carries them.
    - **For Peter:**
      1. *Decided, #25:* the mixed-sign cell semantics (2F Task 18 review I1 and I2): Peter ruled
         "2a" on 2026-10-13, both-signed-only everywhere, with a warning.
      2. lenet.v's Yosys-side runs exist only as `sweep-xl` jobs on an idle machine (rulings C1 and
         I10 applied).
      3. The IR-6 amendment for a compact at the end of `opt` (point (h)) is withdrawn (#19).

24. **Peter 2026-10-13: initial non-constant value — Parmys behaviour in VTR mode, ignored with
    warning otherwise.** `initial x = <non-constant expr>;` (the micro
    `keywords/initial/initial_multiple_statements.v`, `initial out2 = in;`, which the 2H exit
    harness reported as "equivalent outside x-cones" against its Parmys golden): when the run
    targets the VTR flow it is read as `assign x = <expr>;`, with the located warning "initial
    value of 'x' is not constant: treated as a continuous assign (VTR/Parmys compatibility)";
    otherwise (the general case) the value is ignored with the existing warning and `x` stays x.
    Agent defaults he may override (2D spec v4.30, EL-3, EL-40 / §8.8, §9.2, §17 #85):
    - **The switch** is `read_verilog --vtr-compat`, an option beside `--nolatches`. No existing
      project or pass field says "targets VTR" (`flow` is a stored script path, `arch` a tech
      library or XML, `write_blif --vtr` only the output dialect), so the VTR flow's scripts set
      it: `tools/opt-check/sp-splice`, `elab-equiv --vtr-compat` / `elab-sweep --equiv
      --vtr-compat` (the micro sweep), and the odin3 legs of `tools/vtr-qor`. A `--lib` read
      carries it to its units (`odin3_lib_vtr_compat`). `tools/exit-check` (branch
      feat/2H-splice) is not on main; when it lands, whatever runs its odin3 side must set the
      option (sp-splice already does). The ABI `odin3_elab_options` is unchanged.
    - **The assign applies only when `x` has no other driver.** A plain `=` or `<=` assignment (an
      optional delay is ignored) to a variable or a constant select of one, whose bits no `always`
      block assigns and no continuous assignment, instance, gate or input port drives. Otherwise
      the old ignore stays, with a warning that says why ("... is also assigned in an always
      block (VTR/Parmys compatibility does not apply)"). Yosys 0.55 (and so Parmys) stops with
      `Failed to get a constant init value` on the always-assigned case; Odin III warns and goes
      on. `if`/loop statements, concatenation and memory-word targets, and variable
      initialisers (`reg r = in;`) keep the ignore.
    - Evidence: `elab-equiv --techlib lib/vtr.o3lib` on the micro is NOT equivalent without the
      option (`out2`: 0 against Yosys's `in`) and equivalent with it; the Phase 2 script's BLIF
      with the option is equivalent (`equiv-check`) to the Parmys golden. `initial_multiple_blocks.v`
      (`initial out1 = in1;` and `out2 = in2;`) is the same case and is fixed the same way.

25. **Mixed-sign cells: signed only when both flags are set** (Peter, 2026-10-13, ruling "2a" plus a
    warning; 2F Task 18 review I1 and I2; spec 2F §17 #57, supersedes #55; branch
    `feat/2F-mixed-sign`). A word cell with `A_SIGNED != B_SIGNED` is signed only when BOTH flags are
    set, else both operands are zero-extended: Yosys `simlib.v`, the registry evaluator, `merge.c`,
    `share.c` and Verilog's own rule. Yosys's `calc.cc` and some of its `opt_expr` rules extend
    each operand by its own flag; Odin III no longer does anywhere.
    - **Rules changed** (one helper, `odin3_opt_both_signed` in the opt kit; `odin3_expr_both_signed`
      for the rules): `bitwise_logic_one_const`, `fine.group` (`$not` keeps its one flag),
      `mul_to_shift` (the shift extends by the cell's signedness; a one bit that is a constant's sign
      bit is a power of two when the cell is unsigned), `mul` low zeros, `div_to_shift` (logical shift
      on a mixed cell), `divmod` (`$mod` mask and signed forms), `identwrt` (positive wiring and the
      -1 forms), `fine.addsub`, and the constant fold (`fold.c`, every binary operator but `$pow`).
      `identwrt` and `fine.addsub`, made conservative in §17 #55, rewrite again. Cells the rules
      build carry equal flags. Shifts, `$shiftx` and `$pow` are untouched (B's flag is the amount's).
    - **Warning** (WARN, spec 2F §16): `opt: module 'm': cell '$sub' 'c' has A_SIGNED != B_SIGNED:
      treated as unsigned (both-signed rule, Yosys simlib; PHASE2 #25)`. It fires at the first
      `opt_expr` visit of the cell, once per cell per `opt` run (per call outside the loop). There is
      no `read_rtlil` and BLIF has no word cells, so there is no read-time point; `opt_merge` and
      `opt_share` only keep mixed cells apart and say nothing.
    - **Evidence.** RED first: `test_mixed_*` in `tests/unit/test_opt_expr.c` fail on the old rules
      (the registry evaluator's trace differs after `opt_expr` for the bitwise, group, mul and fold
      cases, `$div`/`$mod` give a sign-extended Y, and the identwrt / addsub cases stay); GREEN after.
      `$div`, `$mod` and `$pow` have no simulate hook, so there the ruling itself is the arbiter
      (Y against C's unsigned division). `fuzz_opt` draws A_SIGNED and B_SIGNED apart again; the two
      mixed-sign repros stay in `tests/fuzz/fuzz_opt_regress`. The Phase 2 script's output on 20
      micros and mcml is byte-identical before and after: the front end unifies the flags, so no
      mixed cell reaches `opt`.
