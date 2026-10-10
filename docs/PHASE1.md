# Odin III — Phase 1: core IR and netlist round trip

Spec: `docs/DESIGN.md` §5, §10, §12. Exit test (spec §12): BLIF→IR→BLIF identical on the goldens
(decision #2); the simulator matches ABC on the goldens; a Python plugin can walk the IR.

Each sub-project gets its own spec (`docs/specs/`), plan, and PRs. Model/effort per
`docs/PHASE0.md` §9; the agent says when to switch.

## Sub-projects (decision #1)

| # | Sub-project | Depends on | Spec | Review | Model/effort |
|---|---|---|---|---|---|
| 1A | `util/`: arena, vec, hashmap, str, log | — | `specs/2026-10-08-1A-util-design.md` | light (#6) | Opus `high`; implementers Sonnet |
| 1B | IR core, op registry, `check`, provenance lineage, `docs/IR.md` | 1A | — | **Peter, full** (spec §12) | Fable `xhigh`, advisor on |
| 1G | Tech library format + reader (target-agnostic gate lib first) | 1B | — | Peter (spec change, #8) | Opus `xhigh` → `high` |
| 1C | BLIF reader/writer; round-trip exit test | 1B, 1G | — | light | Opus `high` |
| 1D | Pass manager, C ABI v0, Python plugin walk | 1B | — | Peter (ABI) | Opus `high` |
| 1E | Simulator; "matches ABC" exit test | 1C | — | light | Opus `high` |
| 1F | dot / JSON / Verilog writers | 1C | — | light | Opus `high`; Sonnet for bulk |

1D, 1E and 1F can run in parallel once 1C lands.

## Checklist

- [x] 1A spec approved (2026-10-08, PR #11), implemented, merged (benchmark, release build: 2M interns 0.36–0.45 s, 2M `u64map` puts 0.35–0.39 s, 2M `pagevec` pushes 0.01 s, max RSS 176 MiB)
- [x] 1B `docs/IR.md` (agent-decided, PHASE1 #9; Peter reviews afterwards); IR implemented; 2M-node benchmark recorded (`bench_ir`, release, 2M nodes / 2.2M nets, every node and net with a source record: build 0.835 s, check FULL 0.741 s, fanout walk 0.313 s, delete 10% 0.098 s, compact 0.393 s, `odin3_prov_index_build` 0.064 s (168,064,168 bytes; 2,001 records, 334,660 tombstones), one `odin3_prov_index_by_loc` on the busiest line 0.341 ms (200,000 hits), `odin3_prov_sources` over all 1.8M live nodes 0.060 s, max RSS 810,540 KiB = 791.54 MiB)
- [x] 1G tech-library format approved (PHASE1 #7/#8); reader + generic gate library + VTR golden library
  (2026-10-09): golden round trip with `--techlib lib/vtr.o3lib` identical to 1C's (1846 text ok;
  identical 1814 + stubs 10; exceptions as #15)
- [x] 1C BLIF round trip identical on every `ok` golden (2026-10-09): normalized text identity
  1846/1846; `netlist-compare` identical 1824 (10 with stub models), exceptions per #15 and the
  results below
- [x] 1D a Python plugin walks the IR through the C ABI (2026-10-10): CTest `python_walk` reads
  all 141 committed BLIF fixtures through cffi; per-module counts and cell-type histograms
  identical to the C `stats` pass on every one (Release 141/141; Debug 140, the multi-driver
  original fails the always-on check); results below
- [x] 1E simulator matches ABC on the goldens without RAMs (2026-10-09): `tools/sim-check`, seeds
  1–3 × 64 cycles, every simulated golden identical to the reference (1626 files, 626 distinct
  contents: ABC 405, Yosys 221); 220 excluded (RAM 206, implicit black box 10, multi-driver 4)
- [x] 1F writers emit dot / JSON / Verilog for the goldens (2026-10-09): JSON identical through
  Yosys on 1796/1846, Verilog equivalent 1428 (404 excluded, listed), Icarus parses every
  finished one, dot accepted or budget-refused on all; not validated at the per-tool caps: 38
  files in the JSON step and 14 in the equivalence step (timeouts, memory caps, 2 Yosys
  memory deaths; the large Odin II designs, named below); results below

## Decisions log

2026-10-08, Peter (numbers match the question list):

1. Phase 1 is split into the sub-projects above, in order 1A → 1B → 1C, then 1D/1E/1F in
   parallel. (1G was added by #4 below.)
2. "BLIF→IR→BLIF bit-identical" means identical under `tools/netlist-compare`, with every name
   kept exactly and model/port/cell order preserved. Byte-identity is a stretch goal, not a gate.
3. No raw pointers needed where hashing/IDs reach the object. Provenance must keep the lineage
   through every transformation (AST optimizations, clumping, decomposing); nothing is cleaned
   up, and one can navigate from any piece to the pieces that made it. Agent proposal for 1B:
   an append-only, hash-consed **lineage DAG** of provenance records (sources, creating pass,
   parent records), one 32-bit provenance ID per node/pin/net, backward and forward navigation.
4. Peter wants a target-agnostic **tech-library** (`read_arch`) format: gates only, like a
   techlib, so the flow can map to plain logic first; FPGA targeting grows from it. Agent
   proposal: sub-project 1G in Phase 1 (spec change pending, #8).
5. Memory layout: array-of-structs per object kind in per-module arenas behind accessors; a
   2M-node synthetic benchmark in 1B decides whether to move to struct-of-arrays.
6. 1A gets a light review (agent merges after critique); 1B's `docs/IR.md` gets Peter's full
   review.

9. **IR decisions delegated** (Peter, 2026-10-08 ~17:00): "Make your best decisions for the IR
   and don't worry about the update — you should be able to push further." The agent writes
   `docs/IR.md` (decisions IR-1…IR-18), has it critiqued, and implements 1B without waiting for
   Peter's review; he reviews afterwards and may override any IR-n decision. Spec §12's human
   review of IR changes is satisfied after the fact for Phase 1.

2026-10-08 evening, Peter (answers to the numbered list):

7. **Confirmed:** own text format `.o3lib`; cell functions are Verilog-like expressions compiled
   to IR fragments; Liberty/genlib/VPR-XML importers later (`docs/specs/2026-10-08-1G-techlib-design.md`).
8. **Yes:** 1G is part of Phase 1; spec §6 step 7, §7, §12 and §13 #1 amended (PR #15).
10. Order: 1C (BLIF round trip, using the goldens' declared black boxes) before 1G; 1C adopts
    `vtr.o3lib` when 1G lands.
11. 1D (public C ABI for the IR) is delegated like the IR: the agent designs and merges it,
    Peter reviews afterwards (ABI is 0.x, free to break before a release).
12. The agent continues through 1C, 1E and 1F without stopping, with per-task reviews and
    self-merge on green.
13. 1E exit test: random input vectors; our simulator's outputs compared cycle by cycle against
    Icarus Verilog running ABC's Verilog dump of the same BLIF; goldens with RAMs excluded until
    Phase 4.

14. **Tombstones keep more** (Peter): besides module, kind, cell type, name and provenance, a
    tombstone keeps a dead node's parameters and the names of the nets its pins were connected
    to (IR-6 amended; implemented in a small IR follow-up after 1C).
15. 1C exit test: every `ok` golden reads, round-trips identical under `netlist-compare` with
    names and order kept; `check` errors are allowed only on the four Odin II goldens whose
    netlists drive one net from several cells (`elsif_both_defined`, `multi_assignment`, both
    arches), listed explicitly in the round-trip script.
16. Undeclared, unregistered `.subckt` cells (Yosys `$pow`, `$_DFFSR_PPP_`) become implicit
    black boxes with ports from the file (scalar, width 1, inout); no `.model` is written for them.
17. Order after 1C: 1G, then 1E, 1F, 1D. The full golden round trip runs locally (CI has no
    goldens) via a rerunnable script; CI checks the committed fixtures.
18. **Project input** (Peter, 2026-10-09; DESIGN §4.0): one project record feeds every reader
    (files with language and library, include paths, defines, top and its parameter overrides,
    the architecture (tech libraries / arch files, plus what it offers: per-cell inventory from the
    device or the project, research overrides `hide`/variant cells/project-local `.o3lib`, an
    architecture report), partial-mapping rules (scoped map/soft/keep,
    thresholds, budgets, project patterns; source attribute > instance > module > global > arch
    default), optionally the flow script), filled from a native `.o3proj`, an EDA `-f` file list, a Quartus `.qsf`/`.qpf`
    import, or (later) an Odin II XML config. `.o3proj`, `-f` and `.qsf` land in Phase 2 with the
    Verilog front end (mapping rules read and stored; their semantics land with the Phase 4
    partial mapper). Phase 1 part: 1D adds `odin3_design_set_top`/`top` and top selection
    (`--top`, else the single uninstantiated module, else an error listing the candidates); the
    BLIF reader's "first model is the top" stays the BLIF rule (BLIF defines it) but is recorded
    through the same call.
19. **Project-input test corpus** (Peter, 2026-10-09: "make tests of different projects …
    multiple files, etc. to test the project path, also with the different formats that will
    be supported"): `tests/golden/projects`, 55 small cases (36 positive, 19 negative), each in
    every format that can express it (`.o3proj`, `-f`, `.qpf`/`.qsf`/`.qip`, Odin II XML), with
    the normalized record, the resolved design or located error, and a Yosys (GHDL for VHDL)
    `ref.blif`. `tools/project-fixtures` holds the stdlib reference parsers (the oracle for the
    C readers); CI runs its `check`, `oracle` runs locally. The grammar is normative in DESIGN
    §4.0. Fix round 1 after review (controller rulings, agent defaults Peter may override;
    **(P)** = flagged for Peter's review):
    - Phases: every case parses to its record from `.o3proj`, `-f`, `.qsf` in Phase 2; it
      elaborates and meets its oracle in its `phase` (2 Verilog/BLIF, 5 SV/VHDL/mixed, 6
      VQM); the Odin II format joins when its reader lands. §12 Phase 2/5/6 exit tests say so.
    - No leniency: double-quoted words with `\"`/`\\` escapes only, no word joining, no
      empty quoted word; the `.qsf` reader is Tcl-exact for the subset (no `$`/`[...]`
      substitution except the `.qip` `qip_path` idiom); per-assignment `.qsf` options
      (`unknown_option` otherwise), one value per assignment; case-insensitive only where
      Quartus is (assignment names, enumerated values).
    - `VQM_FILE`/`EDIF_FILE` are design files; `QIP_FILE` is read (nested, `qip_cycle`).
    - Closed behaviours: one `top` (`top lib.name` allowed; `TOP_LEVEL_ENTITY` repeated: last
      wins); a file listed twice in one library is read once with an info line (a diamond of
      nested lists is not a duplicate module); default `+libext+` is `.v`; unknown extensions
      are errors; lookup design → `-v` (first file) → `-y` (first directory, then first
      extension), library modules never shadow design units; Odin II `<input_type>` single,
      `verilog`|`blif`; a `.qsf` can be the entry; `param` only on the top or an instance under
      it, quoted = string, else a number literal; VHDL analysed by dependency; full error-kind
      list in §4.0; readers record paths as given + absolute, the harness relativizes.
    - **(P)** `-f <list>` entries resolve against the outermost list's directory (the standard
      result when run from there, still CWD-independent); `-F <list>` against that list's
      directory. Every other format resolves against the naming file's directory.
    - **(P)** A `.qpf` with several revisions and no `--revision` is `ambiguous_revision`
      (`.o3proj` `import <qpf> revision <r>` also chooses); `SEARCH_PATH` is an include and a
      library directory; `USER_LIBRARIES` a library directory.
    - **(P)** `-f` accepts `-top`/`--top-module`, `-sv`, `+libext+` without `-y`,
      `$VAR`/`${VAR}` from the environment (`undefined_variable`), CRLF.
    - **(P)** Odin II `<optimizations>`: `multiply size` → `map * $mul to multiply min_width`,
      `adder threshold_size` → `map * $add to adder min_width`, `memory split_*` → the new
      `split` rule; the rest ignored and listed.
    - Agent decisions within the rulings: `.qsf` `-hdl_version` accepted and listed as ignored
      (the standard follows the language); `VERILOG_INCLUDE_FILE` ignored; `set_parameter -to`
      paths and style-assignment scopes are anchored at `-entity` or the top; `.o3proj` gains
      `split` and `import`; record `duplicates`; `resolved.order`; `ref.cmd` records tool
      versions; `vhdl_same_entity` uses a hand-written Verilog reference.
    - Fix round 2 (controller rulings after re-review):
      1. Real `.qsf` files parse: `[`/`]` inside a word (bare or quoted) are ordinary
         (`-to LEDR[0]`); only a word starting with `[` is substitution (the `.qip` idiom,
         else an error). Case `qsf_bus_pins`; a test parses Yosys's DE2i-150 `.qsf` when
         `external/` is present.
      2. `-sv` ends with the nested list that says it (restored after the nested parse).
      3. `unknown_top`, and `ambiguous_top` for a declared top, are located at the declaring
         line (`top`, `-top`, `TOP_LEVEL_ENTITY`).
      4. `unsupported_language` is defined in §4.0; the §12 Phase 2 exit test excludes the
         Odin II format, as the README does; `-top`, `$VAR` and CRLF moved to the phase-2
         case `f_env_crlf` (`-sv` stays in the phase-5 `f_options`).
      5. `#` starts a comment only at the start of a word (`+define+D=#1` keeps its value);
         `-f` accepts `+define+TAG="AB"` (value `"AB"`, quotes kept); a backslash is ordinary,
         Windows `\` paths are not translated.
      6. Plain relative paths in a `.qip` resolve against the project directory (Quartus);
         the `qip_path` idiom against the `.qip`.
      7. VHDL order also counts an architecture or package body in another file than its
         entity or package (case `vhdl_split_units`); VQM instances resolve only against a
         small device-primitive list (else `unresolved_module`); RTL megafunctions
         (`altsyncram`, `lpm_*`, …) resolve through the Phase 2 primitive library (§4.0).
      8. Negatives for `include_cycle`, `qip_cycle`, `dependency_cycle`, `unknown_revision`,
         `unsupported_input_type`; cases `param_instance` (instance-scoped) and
         `param_string` (string parameter).
      Agent detail: `unknown_revision` is located at the `import` line, unlocated for
      `--revision`; files a scan read before an error count as used.
    Phase 2 exit test (§12) gains the project-fixture clause.

2026-10-09, agent defaults (1G Task 4; Peter may override):

20. **IR-7b matches a black-box declaration to a registered type by port name, not position.**
    The goldens hold 49 distinct `.model … .blackbox` stanzas for four models: Yosys+Parmys lists
    one model's ports in a different order from file to file, and Odin II spells width-1 ports
    `a[0]` where Parmys writes `a`, so no library port order can match positionally. Widths may be
    parametric: the declared widths give the parameters (a port sized by `A_WIDTH` sets it), and
    every port's width rule must then reproduce its declared width. The declared-model entry keeps
    the declaration as written (port order, widths, scalar flags) and the inferred parameters;
    instances use the declaration's spelling and parameters, and the writer reproduces both.
    `lib/vtr.o3lib` lists ports in Odin II's order (the one fixed order in the goldens).
21. **`.subckt` parameters without a `.model`:** an instance of a registered type the file does not
    declare gets the parameters its formals imply (each port as wide as its largest bit index + 1);
    implicit black boxes (#16) still apply only to names nothing registers. The writer refuses a
    cell whose parameters the reader would not derive back.

2026-10-09, agent defaults (1F Task 4; Peter may override):

22. **(P) 1F JSON validation compares through Yosys on both sides** (spec deviation, accepted by
    the controller's ruling; for Peter's review). The spec's "Yosys `read_json; write_blif` →
    `netlist-compare` identical to the golden" cannot hold literally: Yosys `write_blif` spells
    every `$lut` as minterms, folds constant aliases and writes a 1-bit wire's bit without its
    index. `tools/writer-check` therefore compares the JSON through Yosys (`read_json`; NORM;
    `write_blif`) with the golden through Yosys (`read_blif`, `-sop` when a cover has over 12
    inputs; NORM; `write_blif`): NORM makes identity `$lut`s connections (as `read_blif` does)
    and runs `opt_clean`; both outputs then get covers of at most 12 inputs as minterms,
    statements sorted and Yosys self-buffers dropped, and the JSON side's 1-bit names get their
    `[0]` back. Each normalization has a fixture that fails without it (table in
    `tests/golden/blif/README.md`). A multi-driver refusal is accepted only for the #15 list.
    Goldens Yosys cannot read, or with a `.subckt $<lowercase…>` cell and no `.param` line (the
    8 parameterless `$pow` goldens, which `write_blif` drops on both sides), are excluded from
    the JSON step and listed.
23. **(P) 1F Verilog validation exclusions** (spec deviation: the spec excludes only RAM goldens;
    agent default accepted by the controller's ruling, for Peter's review). `equiv-check` models
    no black box but the VTR `adder`, no multi-driver net and one clock domain: goldens with
    RAMs, multipliers, implicit cells, multi-driver nets or several clocks are excluded from the
    equivalence step (listed by reason). Only an `equiv-check` error located in the golden file
    excludes; any other is an error (CTest `writer_check_bbox_not_excluded`). Icarus still
    parses every Verilog output. Assign-form cells carry their attributes as a `// (* … *)`
    comment (Icarus rejects attributes on continuous assigns).
24. **(P) dot validation** (spec deviation for 92 goldens; agent default accepted by the
    controller's ruling, for Peter's review): `dot -Tsvg` within 120 s; a layout that takes
    longer (2000-node budget, dense graphs) is checked with Graphviz's parser `nop` instead
    ("ok-parse"); a design over the budget must be refused with its node count ("budget").
25. **IR follow-up `odin3_attr_foreach`** (read-only, first-set order; docs/IR.md IR-10) lands
    with 1F so the JSON and Verilog writers write every attribute; for Peter's review.

## 1C results: full golden round trip

Run 2026-10-09 on `~/odin3-ws/golden` (all `status=ok` BLIFs), release build, writer at
`b68d4c1`, script at `4f55d5a` (`tools/blif-roundtrip/blif-roundtrip -j 2 -t 600`), 51 min 29 s
wall clock. Gates: normalized text identity (continuations joined, comments stripped, blanks
collapsed, empty header lists dropped, `.subckt` formals sorted, repeated `.attr`/`.param` keys
reduced to the last, default latch init explicit) and `netlist-compare` with stub `.blackbox`
models for implicit cells. Files with identical content run `netlist-compare` once (724 distinct
contents of 1846). `netlist-compare` runs under a 600 s timeout and a 6 GB address-space cap.

```
files: 1846 (structural gate run on 724 distinct contents)
text gate: 1846 ok, 0 FAIL, 0 read/write failures
structural gate: identical 1814, identical+stubs 10, refused (multi-driver original) 4, timeout 16, memlimit 2, different 0, error 0, not run 0
check FULL errors: 0 files outside the multi-driver list, 4 inside it
slowest file: EArch/regression/verilog/large/LU64PEEng/LU64PEEng.odin.blif (634.4 s total)
peak RSS: odin3-blif-rt 2314 MB (EArch/regression/verilog/large/LargeRam/LargeRam.odin.blif); netlist-compare 6061 MB (EArch/regression/verilog/large/LargeRam/LargeRam.odin.blif)
RESULT: PASS
```

The "slowest file" above is the 600 s `netlist-compare` timeout, not reader/writer speed. The
reader/writer alone (`odin3-blif-rt`: read, check FULL, write; release build, timed by hand
afterwards, since the run did not record it separately): `LargeRam.odin.blif` (915 MB) 16.8 s at
2.37 GB peak RSS, `LU64PEEng.odin.blif` (147 MB) 1.9 s at 311 MB, `mcml.odin.blif` (61 MB) 0.8 s at
189 MB, `vtr/bgm.odin.blif` (34 MB) 0.4 s at 101 MB; time is linear in file size. Later runs of the
script also report the `odin3-blif-rt` total and slowest file.

Exception lists (reported, not failures):
- Multi-driver originals, refused by `netlist-compare` (decision #15), 4 files: `elsif_both_defined`
  and `multi_assignment`, `.odin.blif`, both architectures. These are also the only 4 with check errors.
- Timeouts at 600 s, 16 files (6 distinct contents, repeated across architectures): Odin II
  `LU8PEEng` (2 contents: `full`, `vtr`), `LU32PEEng`, `LU64PEEng`, `bgm` (`large` and `vtr`). All
  pass the text gate.
- Memory cap (6 GB), 2 files with one content: Odin II `LargeRam` (915 MB). Passes the text gate.
- Implicit cells compared with stub models (identical): 10 files (`pow`, `pow_const`, `dffsre`,
  `twobits_arithmetic_power`, `eightbit_arithmetic_power`, both architectures).

## 1G results: golden round trip with `lib/vtr.o3lib`

Run 2026-10-09 on `~/odin3-ws/golden` (all `status=ok` BLIFs), release build of the 1G Task 4
tree, `tools/blif-roundtrip/blif-roundtrip -j 2 -t 600 -m 6 --techlib lib/vtr.o3lib` (smallest
file first), 52 min 5 s wall clock. Every golden's `adder`, `multiply`, `single_port_ram` and
`dual_port_ram` now resolve to the library's hard cells (IR-7b, decisions #18–#19) instead of
local black boxes; the output must still be identical.

```
files: 1846 (structural gate run on 724 distinct contents)
text gate: 1846 ok, 0 FAIL, 0 read/write failures
structural gate: identical 1814, identical+stubs 10, refused (multi-driver original) 4, timeout 16, memlimit 2, different 0, error 0, not run 0
check FULL errors: 0 files outside the multi-driver list, 4 inside it
slowest file: EArch/regression/verilog/large/LU64PEEng/LU64PEEng.odin.blif (630.4 s total)
odin3-blif-rt (read, check, write) time: 85.2 s total, slowest k6_frac_N10_frac_chain_mem32K_40nm/regression/verilog/large/LargeRam/LargeRam.odin.blif (17.3 s)
peak RSS: odin3-blif-rt 2356 MB (k6_frac_N10_frac_chain_mem32K_40nm/regression/verilog/large/LargeRam/LargeRam.odin.blif); netlist-compare 6081 MB (EArch/regression/verilog/large/LargeRam/LargeRam.odin.blif)
RESULT: PASS
```

The exceptions are exactly 1C's (same 4 multi-driver files, 16 timeouts, 2 memory-cap stops); no
other difference. The goldens' 49 distinct black-box stanzas are committed as fixtures
(`tests/golden/techlib`, `tools/golden-blackboxes`) and round-trip in CTest
`blif_roundtrip_techlib`.

1C follow-ups (minor, from the task and final reviews; none changes a result on a golden):
- Reader: `#` inside a quoted `.attr`/`.param` value is cut as a comment; an empty or
  comment-only file reads as an empty design with `ODIN3_OK`; a `\` as the final byte becomes a
  literal token; a model with both scalar `a` and vector `a[k]` ports reads but cannot be
  instantiated; `list_extra` is quadratic in attributes per cell; `find_port` is O(P²) per
  implicit type.
- Writer: needs a const module accessor in the IR (one cast today); an in+out same-name
  `.subckt` with only the output-side pin connected reads back bound to the input port.
- Gate: the negative CTest uses `WILL_FAIL` (passes on any failure; match the gate's message);
  the report counts result files rather than the work list; 9 committed fixtures where the spec
  said "a few dozen".

## 1E results: simulator against the reference, every `ok` golden

Run 2026-10-09 on `~/odin3-ws/golden` (all `status=ok` BLIFs), release build at `144aad7`,
`tools/sim-check/sim-check -j 2 -t 600 -m 6` (smallest file first; `--techlib lib/vtr.o3lib`
by default), 33 min 9 s wall clock. Per golden and per seed 1, 2, 3, `odin3-sim-vectors` runs
64 cycles of random vectors; the reference replays the same inputs on a Verilog dump of the same
BLIF under Icarus Verilog 12.0 (testbench from `tools/sim-check/tb.sh`). The two outputs must be
identical line by line (`cycle inputs outputs`, every primary output bit, every cycle). Files with
identical content run once (724 distinct contents of 1846).

The reference netlist: ABC `read_blif; write_verilog` (PHASE1 #13) for pure logic whose
registers all clock on the rising edge; Yosys `read_blif -sop -wideports; simplemap t:$sop;
write_verilog -noattr` plus behavioural models `tools/sim-check/models/{adder,multiply}.v`
(written from the `lib/vtr.o3lib` definitions as `{cout,sumout} = a + b + cin` and unsigned
`a * b`) for goldens with `adder`/`multiply` instances, falling-edge registers, or a data input
named `clock` (agent ruling extending #13: ABC turns black boxes into cut points, drops the latch
clocks and clocks every register on one rising `clock`). Latches with init 2/3 or none get init 0
in the reference's copy of the BLIF, as the simulator starts them (PHASE0 #7).

```
files: 1846 (simulated or classified once per distinct content: 724)
pass 1622, FAIL 0, sim-error 0, ref-error 0, timeout 4, memlimit 0, excluded 220
excluded: implicit black box 10, multi-driver netlist 4, RAM 206
reference (distinct contents run): abc 405, yosys 221
slowest file: EArch/vtr/bgm/bgm.odin.blif (654.1 s total)
peak RSS: odin3-sim-vectors 141 MB (EArch/vtr/bgm/bgm.parmys.blif); reference 2049 MB (iverilog, EArch/vtr/bgm/bgm.parmys.blif)
RESULT: PASS
```

The 4 timeouts are Icarus compiling the Yosys dump of Odin II's `bgm` (2 distinct contents, `large`
and `vtr`, each in both architectures; 54 MB of Verilog), not our simulator (4.6–5.0 s for three
seeds). Rerun with a 1-hour cap (`-t 3600`), both pass (excerpt: the table header and the
excluded/reference/slowest-file summary lines are omitted):

```
pass      yosys   1793.0  EArch/regression/verilog/large/bgm/bgm.odin.blif
pass      yosys   1791.1  EArch/vtr/bgm/bgm.odin.blif
pass 2, FAIL 0, sim-error 0, ref-error 0, timeout 0, memlimit 0, excluded 0
peak RSS: odin3-sim-vectors 120 MB (EArch/vtr/bgm/bgm.odin.blif); reference 2676 MB (iverilog, EArch/vtr/bgm/bgm.odin.blif)
```

So every golden without RAMs or implicit cells simulates and matches, except the 4 multi-driver
files. `odin3-sim-vectors` totals 67.2 s for all 626 simulated contents × 3 seeds (slowest
`vtr/bgm.odin.blif`, 5.0 s; peak 141 MB). Exclusions (listed, not failures):
- RAM instances (`single_port_ram`, `dual_port_ram`; Phase 4), 206 files.
- Implicit black boxes (#16), 10 files: `$pow` (`pow`, `pow_const`, `twobits_arithmetic_power`,
  `eightbit_arithmetic_power`) and `$_DFFSR_PPP_` (`dffsre`), both architectures.
- Multi-driver Odin II netlists (#15), 4 files (`elsif_both_defined`, `multi_assignment`, both
  architectures): the simulator refuses a net with two drivers, and so does the reference (ABC:
  `Signal "simple_op^out" is defined more than once. Reading network from file has failed.`).

Mismatches found while building the harness were all in the harness, none in the simulator (each
is now a test in `tests/tools/test_sim_check.py`): latch init rewriting miscounted fields;
Yosys refuses a `.names` of more than 12 inputs unless read with `-sop` (then `simplemap t:$sop`;
`models/sop.v` stands in for Yosys < 0.45, e.g. CI's apt 0.33); joined continuation lines kept a
leading blank Yosys rejects; a testbench clock `reg` steps x → 0 at time 0, a negedge that clocked
falling-edge registers early (the clock is now a `tri0` net forced high and released). CTest
`sim_check_fixtures` runs the comparison on `tests/golden/blif` and `tests/golden/techlib` (CI
installs `iverilog`, `yosys` and `time`). On a machine without ABC, Yosys or Icarus the CTest is
not registered (a CMake STATUS line says so) and the sim-check Python tests skip; CI is the guard.

Gate fixes after the run (task review; the recorded outcomes do not change, since all 4 timeouts
were Icarus): a timeout or memory-cap stop of `odin3-sim-vectors` is now a failure in every mode
(only reference-tool timeouts are exceptions); both sides must print exactly 64 cycle lines per
seed, and the testbench's cycle count comes from sim-check, not from the driver's header; the
driver's clock bits must equal the BLIF's latch clock nets; multi-driver files are excluded when
the simulator rejects them and ABC refuses them too (no basename list); `-j` is 1 or 2. The
summary now reads `… reference timeout N, reference memlimit N, excluded N, missing N` and counts
the references of compared contents only.

`fn` cell cost (ruling: per-call resize measured here): callgrind on `vtr/stereovision2.odin.blif`
(540 `multiply`, 14,347 `adder`, 12,103 `.names`), 16 cycles: 729,463 `fn` hook calls average
3,418 instructions each (inclusive), of which `size_all` (re-sizing the expression nodes on every
call) is 1,235 and `odin3_word_extend` 230, so about 43% of the call; a `.names` hook call averages
128. The `fn` hooks are 59% of all instructions and 97% of the cycle time; 64 cycles take 0.66 s
after a 0.16 s build. Worth caching the sizes at build time when simulation speed matters.

## 1F results: writers on the goldens

Run 2026-10-09 16:16–19:54 (3 h 38 min wall clock) on `~/odin3-ws/golden` (all `status=ok`
BLIFs), `tools/writer-check/writer-check -j 2 -t 600 -m 6` (script at `5f4e57e`, release
`odin3-write` of the same tree, smallest file first, identical contents once), then the files
the later script changes touch rerun with `--resume` (script `07ebc66`: the 8 `$pow` goldens
move from "different" to "ref-excluded", decision #22). After the rebase onto 1G
(`width_expr` ports; reader changes for registered types), the rebased `odin3-write` writes
byte-identical JSON, Verilog and dot to the run's binary on every 4th golden under 20 MB. Validations per decisions #22–#24; per-tool cap 600 s and 6 GB address space.

```
files: 1846 (724 distinct contents)
json: identical 1796, ref-excluded 8, refused (multi-driver) 4, different 0, yosys-error 0, error 0, timeout 32, memlimit 4, ref-timeout 0, ref-memlimit 2, write failures 0
verilog iverilog: ok 1828, FAIL 0, timeout 16, memlimit 2
verilog equiv: equivalent 1428, excluded 404, NOT-equivalent 0, yosys-error 2, error 0, timeout 4, memlimit 8
dot: ok 1486, ok-parse (layout over --dot-timeout; Graphviz nop accepts it) 92, budget (refused over --max-nodes) 268, FAIL 0, timeout 0, memlimit 0
slowest file: EArch/regression/verilog/large/LU64PEEng/LU64PEEng.odin.blif (1384.1 s total)
odin3-write (read + three writers) slowest: EArch/regression/verilog/large/LargeRam/LargeRam.odin.blif (46.5 s)
peak RSS: odin3-write 3350 MB (EArch/regression/verilog/large/LargeRam/LargeRam.odin.blif); tools 6141 MB (EArch/regression/verilog/large/LargeRam/LargeRam.odin.blif)
RESULT: FAIL (2 failing items)
```

The two failing items are one content (`LU64PEEng.parmys.blif`, both architectures): Yosys
`read_verilog … write_blif` on its 336 MB Verilog died without a message at the 6 GB address-space
cap (worker peak 6.19 GB); under a 2 GB cap the same command ends in `std::bad_alloc`. The
script now counts a signal death within 10% of the cap as `memlimit` (`07ebc66`); not a writer
fault. Not failures (reported): timeouts and memory-cap stops are the large Odin II designs
(`LU8/32/64PEEng`, `bgm`, `mcml`, `or1200`, `boundtop`, `paj_*_hierarchy_no_mem`, `sha`,
`LargeRam`), as in 1C; JSON `refused` is the multi-driver list (#15); every Icarus parse that
finished passed; no JSON comparison differed and no Verilog output was NOT equivalent.

Excluded from the Verilog equivalence step (equiv-check cannot model the golden; 404 files, by
reason, `.odin`/`.parmys` leaf names):
- `multiply` black box (170): mult, mult_const, pow_const.odin, ansiportlist, ansiportlist_2,
  binops, cf_fft_256_8, cf_fft_1024_16, cf_fir_24_16_16, cf_fir_3_8_8, diffeq1, diffeq2,
  diffeq_f_systemC.odin, diffeq_paj_convert, fir_scu_rtl_restructured_for_cmm_exp, iir1,
  iir_no_combinational, matmul.odin, oc54_cpu, stereovision1, stereovision2, bgm,
  paj_raygentop_hierarchy_no_mem.odin, paj_top_hierarchy_no_mem.odin, raygentop.odin,
  raygentop_nolatches.odin, sv_chip1/2_hierarchy_no_mem, bm_arithmetic_unused_bits,
  bm_base_multiply, bm_functional_test, bm_match1..6_str_arch, param_override,
  eightbit_arithmetic_power.odin, rs_decoder_1, rs_decoder_2.
- `dual_port_ram` (110): 1r2w, 2r.odin, 2r1w, 2r2w, bram, dpram, mem, LU8PEEng, LU32PEEng,
  bm_base_memory, bm_sfifo_rtl, matmul.parmys, mcml.parmys, mkPktMerge, mkDelayWorker32B,
  mkSMAdapter4B, or1200, spree.parmys, bm_simple_memory, both_ram, inferred_DPram,
  memory_combinational.parmys, multi_edge_reader_writer.
- `single_port_ram` (78): 1r.odin, memrd.odin, rom.odin, spram, spram_big, ch_intrinsics,
  ch_intrinsics_nolatches, mcml.odin, memory_controller, stereovision0, arm_core, boundtop,
  boundtop_nolatches.parmys, raygentop.parmys, raygentop_nolatches.parmys, spree.odin,
  constant_module_inst, inferred_ram_w_clog2, matrix_multiplication.odin, memlooptesting.
- several clock domains (32): register, stereovision3, sv_chip3_hierarchy_no_mem,
  multi_clock_reader_writer, multiclock_output_and_latch, multiclock_reader_writer,
  multiclock_separate_and_latch.
- implicit cells without a model (10): `$pow` (pow, pow_const, eightbit_arithmetic_power,
  twobits_arithmetic_power, `.parmys`), `$_DFFSR_PPP_` (dffsre.parmys).
- multi-driver nets (4): elsif_both_defined.odin, multi_assignment.odin.

Excluded from the JSON step (8): the `$pow` goldens above (Yosys `write_blif` drops a
parameterless `$pow` on both sides). dot: 268 designs over the 2000-node budget were refused
with their node count; 92 within it took Graphviz over 120 s to lay out and were checked by its
parser `nop`. Slowest `odin3-write` (read + JSON + Verilog + dot): LargeRam 46.5 s at 3.35 GB.
Full table: `~/odin3-ws/work/writer-run/merged-report.txt` (not committed).

Gate fixtures: CTest `writer_check_fixtures` (11 fixtures) and
`writer_check_detects_corruption`; `hand_writers.blif` and `hand_wide.blif` were added for the
Yosys-side differences the sample run found (none was a writer bug; the writer changes were
declared black boxes as Yosys `blackbox` modules in the JSON and every attribute written).

## 1D results: Python walks the IR

CTest `python_walk` (`tests/tools/test_python_walk.py`, 2026-10-10) reads every committed BLIF
fixture under `tests/` through the cffi binding (`plugins/python/odin3.py`) — 141 files:
`tests/golden/blif` 12, `tests/golden/techlib` 49 (after `read_techlib lib/vtr.o3lib`),
`tests/golden/projects` 36, `tests/tools/fixtures` 43, `tests/cli` 1 — and walks each design from
Python (`plugins/python/walk.py`: modules, live nodes, nets, wires, ports, cell-type histogram).
The walk's lines equal, line for line, the messages the C `stats` pass logs for the same design,
captured through the ABI log sink (not a re-implementation): identical on 141/141 against the
Release library and on 140/141 against the Debug one, where `elsif_both_defined.odin.blif` (a
multi-driver original, decision #15) fails the check after `read_blif` and is not compared. A
walk with one count altered differs on all 11 fixtures it was tried on. The same test walks
pins against nets on every fixture (each connected pin is on its net, each net pin points back,
drivers are OUT/INOUT), provenance both ways (`node.sources()`, `design.objects_at(file, line)`),
parameters, ports and string attributes. It takes about 1 s (Debug, ASan) and is skipped where
cffi is missing; locally it uses the repository's `.venv` Python, and CI's Python has cffi from
`requirements-dev.txt`.
