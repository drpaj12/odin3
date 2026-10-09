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
- [ ] 1C BLIF round trip identical (`netlist-compare`) on every `ok` golden
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

Exception lists (reported, not failures):
- Multi-driver originals, refused by `netlist-compare` (decision #15), 4 files: `elsif_both_defined`
  and `multi_assignment`, `.odin.blif`, both architectures. These are also the only 4 with check errors.
- Timeouts at 600 s, 16 files (6 distinct contents, repeated across architectures): Odin II
  `LU8PEEng` (2 contents: `full`, `vtr`), `LU32PEEng`, `LU64PEEng`, `bgm` (`large` and `vtr`). All
  pass the text gate.
- Memory cap (6 GB), 2 files with one content: Odin II `LargeRam` (915 MB). Passes the text gate.
- Implicit cells compared with stub models (identical): 10 files (`pow`, `pow_const`, `dffsre`,
  `twobits_arithmetic_power`, `eightbit_arithmetic_power`, both architectures).
