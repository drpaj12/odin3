# Odin III: Phase 6: analysis flow and provenance

Spec: `docs/DESIGN.md` §5 (IR, provenance), §9 (writers), §12; ADRs D11 (this phase), D10
follow-up 2 (the goals), D9 (provenance). Inputs: `write_dot` (1F), `write_json` (1F), the
provenance index and `DERIVED` records across every pass including `abc` (PHASE3 decision 14),
`read_blif`/`read_verilog`, Phase 5's `read_json` and provenance read-back, and `tools/vtr-qor`
for VPR runs.

**Goal (Peter, 2026-10-19).** The generalized flow the post-v1.0 research needs: read
gate-level netlists, see them in Graphviz, export them to graph tools, levelize them, and
trace every object of the final netlist back to the HDL. The experiments themselves
(Gate-Breaker redo, Bayesian control/datapath classifier, graphlets for hardware trojans,
graph sandwich, GNNs, the placement-correlation study) come after v1.0 as plugins over these
outputs (D10 follow-up 2).

**Exit test** (DESIGN §12 row 6): items 6.1 to 6.5 pass their tests; the 6.5 gate passes on
the Phase 3 design set and the Phase 5 LPM micros.

## Checklist

- [ ] 6.1 Gate-level netlist ingest (was 4.15): structural Verilog over a cell library (cells
  given as Verilog models through `read_verilog -v`/`-y`, kept as cells or flattened to IR
  primitives) and BLIF (`read_blif`), with hierarchy and names kept. Test: a micro mapped by
  Yosys to a small CMOS cell set (BUF, NOT, NAND2, NOR2, DFF, DFFSR) and the same micro as BLIF
  both read in and are equivalent to the RTL.
- [ ] 6.2 `write_dot --color-by <attr>` and provenance on node labels (was 4.16): nodes
  coloured by the value of a named attribute (a legend per value), the HDL `file:line` on each
  node label when provenance has one. Test: a micro with an attribute set on some cells gives
  the expected colours and labels; the dot parses (Graphviz `nop`).
- [ ] 6.3 Graph export (was 4.17): Yosys JSON (`write_json`) plus versioned Odin III keys per
  cell and net (module path, attributes, level from 6.4, provenance), so Phase 5's `read_json`
  reads it and networkx and PyTorch Geometric load it without Odin III (a small loader in
  `plugins/python/`). Test: export of a micro reloads in Python with the same node and edge
  counts as the IR, and `read_json` reads it back identical.
- [ ] 6.4 `levelize` pass (was 4.18): the combinational level of each cell from inputs and
  register outputs, stored as an attribute; combinational loops reported. Test: levels on
  micros match a hand count; a loop gives a located warning.
- [ ] 6.5 Provenance coverage gate (was 4.19): the share of final-netlist objects (BLIF after
  `abc`, LPM Verilog, VPR block names from the `.place` file, and each after a Phase 5 read-back)
  that resolve to an HDL source location through the provenance chain (`DERIVED` records back
  to `SOURCE` spans, D9). Test (decision 2): every LUT, latch, hard block and LPM instance of
  the final netlist resolves to at least one HDL location, and every VPR placed block's atoms
  do. The only exceptions are named classes (constant drivers, buffers the writers add), each
  counted. An unresolved object outside those classes is a bug. Recorded per design: the
  coverage, and the specificity (median and 90th percentile of distinct HDL lines per object).

## Dependencies

- 6.3 depends on Phase 5's `read_json` (5.3); 6.5's read-back cases depend on 5.4.
- 6.1, 6.2 and 6.4 depend only on what is merged and can start at any time.
- 6.5's VPR part uses `tools/vtr-qor` (heavy-run lock: run when lanes are idle).

## Decisions log

1. **Generalized flow** (Peter, 2026-10-19; was PHASE4 decision 5): "build the generalized flow
   in OdinIII to hit on this". The items serve D10 follow-up 2's goals; the experiments are
   designed after v1.0.
2. **Provenance coverage is a gate** (Peter, 2026-10-19; was PHASE4 decision 7): "4.19 using
   the Provenance path we should expect a very good report". Full coverage apart from named
   exception classes.
3. **Graph export is Yosys JSON plus Odin III keys** (Peter, 2026-10-19, PHASE5 decision 3):
   one format, one reader.
