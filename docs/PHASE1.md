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
- [ ] 1G tech-library format approved; reader + generic gate library + VTR golden library
- [x] 1C BLIF round trip identical on every `ok` golden (2026-10-09): normalized text identity
  1846/1846; `netlist-compare` identical 1824 (10 with stub models), exceptions per #15 and the
  results below
- [ ] 1D a Python plugin walks the IR through the C ABI
- [ ] 1E simulator matches ABC on the goldens without RAMs
- [ ] 1F writers emit dot / JSON / Verilog for the goldens

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
