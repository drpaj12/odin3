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
        - formals inside body strings (§10.4).
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
      in CI). Odin III's `preprocess` against `iverilog -E -g2005 -grelative-include` and Yosys
      `read_verilog -nosynthesis -ppdump`, all run from an empty directory. Outputs are
      normalized alike (PP-10 regions; pass-through, `` `line ``, `` `file_push `` and
      `` `file_pop `` lines) and compared as tokens. `pp-diff --suite` takes 285 files, smallest
      first, mcml.v last: 20 reviewer probes (not `rec.v`), 1 fixture, 234 micros holding a
      backtick, and 31 VTR files. Run with the debug build: 9.6 s, exit 0.
      - *Result:* agree 270, oracles disagree 11, declared 4, undeclared disagreements 0,
        excluded 21. The 270 are 8 probes (v2: all three reject), 231 micros and all 31 VTR
        files (mcml.v: 133,191 tokens). The four `translate_off` benchmarks are among them.
      - *Declared (Odin III's error expected; both oracles accept):* probes t5 and u3 (`directive
        inside macro text`), w1 (`recursive macro 'N'`), and the fixture
        `tests/tools/fixtures/pp_diff/directive_name.v` (`` `define timescale 1 ``).
      - *Oracles disagree (listed, not failed):* t8 and the 3 splicing micros (Icarus
        substitutes nothing for an undefined macro); t6 (Icarus substitutes formals in a
        string); t9 and w5 (a newline before `(`: Yosys rejects); w2 (`` `define F() ``: Icarus
        rejects); t4 (a `//` comment ending in `\`: Icarus continues the `define`); t7 (a
        comment inside a body: Icarus pastes `ab`); u1 (a multi-line `/* */` in a `define`:
        Yosys keeps it; Icarus reports "Unterminated comment in define", exits 0 and goes on;
        Odin III rejects it). Odin III matches Yosys in 8 of these, Icarus in 2 and neither in
        u1.
      - *Excluded:* `probes/rec.v`, where both oracles loop forever (PP-7; Odin III's rejection
        is unit-tested). The 20 micros that another micro includes are compared through that
        includer: the `keywords/.generic` templates, `assign.vh`, `generate_for.vh`, the three
        `generate_endgenerate/*.vh`, `nested_ifdef.vh` and the module files of the
        `*-syntax` micros.
      - *Agent defaults:* `-grelative-include`, so Icarus searches the including file's
        directory first, as Odin III (PP-11) and Yosys do. `-nosynthesis`, so Yosys does not
        define `SYNTHESIS`. A `` `NAME `` left in Yosys's dump counts as a Yosys rejection,
        because its lexer errors there ("undefined macro"). A Yosys parse error after the dump
        does not count. A declared divergence is checked by a substring of Odin III's error.
        A pass-through directive is stripped from its backtick to the end of its line. Based
        literals are compared without their inner blanks. No C fix was needed.
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
