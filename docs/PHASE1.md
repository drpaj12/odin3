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
- [ ] 1B `docs/IR.md` approved by Peter; IR implemented; 2M-node benchmark recorded
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

Open for Peter (asked 2026-10-08, needed at 1G):

7. Tech-library syntax: agent recommends an own small text format (cell function as a boolean
   expression or an IR fragment), with Liberty/genlib and VPR-XML importers later.
8. Spec change: add 1G to Phase 1; update `docs/DESIGN.md` §6, §7, §12 (and §13 #1).
