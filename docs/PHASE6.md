# Odin III: Phase 6: analysis flow and provenance

Spec: `docs/specs/2026-10-21-6-analysis-flow-design.md` (v1, awaiting Peter's approval). Plan:
`docs/plans/2026-10-21-6-analysis-flow-plan.md`.

Design inputs: `docs/DESIGN.md` §5 (IR, provenance), §9 (writers), §12; ADRs D11 (this phase), D10
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
- Note (4.8): `fsm` leaves `odin3_fsm_states`, `odin3_fsm_encoding`, `odin3_fsm_orig_bits` and
  `odin3_fsm_orig_name` on a recoded state wire and its register, for the graph analysis here.
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
  Baseline for the read-back cases: `docs/specs/2026-10-20-5.4-hdl-locations-design.md` section 14
  (`tools/loc-check`, 5.4e): per format and object kind, what survives a write and read, the lost
  objects by class X-1 to X-9, and the specificity before and after.
  - [x] 6.5a `dump_locs --types`; 6.5b `tools/prov-cover` legs a, b and c (PRs merged).
  - [x] 6.5c leg d (VPR placed blocks, `tools/prov-cover/vpr_leg.py`, CTest `prov_cover_sweep_place`):
    built, every placed name maps to a netlist name (0 unmapped, 0 unplaced on the 14 designs).
  - [ ] Gate green: leg b and d lose the location of the driver of an output port bit (D1/D3) in 13
    of 14 designs and, until PR 207 merges, the 8 `single_port_ram` of ch_intrinsics (D2). Decision
    for Peter pending: a counted `port-driver` class, or locations on port bits.

## Dependencies

- 6.3 depends on Phase 5's `read_json` (5.3); 6.5's read-back cases depend on 5.4.
- 6.1, 6.2 and 6.4 depend only on what is merged and can start at any time.
- 6.5's VPR part uses `tools/vtr-qor` (heavy-run lock: run when lanes are idle).

## Numbers (6.5, 2026-10-21, debug build, VTR 12 `vpr`, k6_frac arch)

Per design and leg on the 14 small VTR designs (the Phase 3 set that is tested for equivalence).
Cells are resolved/objects and coverage (after the approved `inherited` and `pad` classes; leg d
objects are placed blocks plus the atoms inside them, pads and the `gnd`/`vcc` generators excluded);
`def` is the defect count; the last column is leg a specificity (median / p90 distinct HDL lines).

| design | leg a | leg b | leg d | a median / p90 |
|---|---|---|---|---|
| single_wire | 0/0 n/a | 0/1 n/a (def 0) | 0/0 n/a (def 0, pad 2) | 0 / 0 |
| single_ff | 1/1 100.0% | 0/1 0.0% (def 1) | 0/2 0.0% (def 1, pad 3) | 1 / 1 |
| and_latch | 2/2 100.0% | 1/2 50.0% (def 1) | 2/3 66.7% (def 1, pad 4) | 1 / 1 |
| multiclock_separate_and_latch | 6/6 100.0% | 2/4 50.0% (def 2) | 4/6 66.7% (def 2, pad 8) | 2 / 2 |
| multiclock_output_and_latch | 11/11 100.0% | 8/11 88.9% (def 1) | 9/10 90.0% (def 1, pad 7) | 1 / 2 |
| multiclock_reader_writer | 32/32 100.0% | 31/34 96.9% (def 1) | 31/32 96.9% (def 1, pad 4) | 2 / 8 |
| diffeq1 | 628/628 100.0% | 532/631 84.7% (def 96) | 563/659 85.4% (def 96, pad 258) | 4 / 4 |
| diffeq2 | 435/435 100.0% | 339/438 77.9% (def 96) | 360/456 78.9% (def 96, pad 162) | 3 / 3 |
| ch_intrinsics | 225/225 100.0% | 144/285 77.8% (def 73) | 154/196 78.6% (def 41, pad 229) | 8 / 8 |
| ch_intrinsics_nolatches | 312/312 100.0% | 157/289 70.4% (def 106) | 163/232 70.3% (def 66, pad 229) | 8 / 8 |
| stereovision3 | 320/320 100.0% | 292/324 91.2% (def 28) | 304/332 91.6% (def 28, pad 41) | 2 / 8 |
| sha | 2503/2503 100.0% | 2467/2506 98.6% (def 36) | 2598/2634 98.6% (def 36, pad 74) | 3 / 3 |
| mkPktMerge | 1394/1394 100.0% | 701/860 99.6% (def 3) | 706/709 99.6% (def 3, pad 467) | 2 / 4 |
| blob_merge | 10542/10542 100.0% | 10305/10408 99.4% (def 61) | 10496/10557 99.4% (def 61, pad 136) | 1 / 8 |

Leg c (LPM micros, `tests/lpm`, 8 files): 100% resolved in memory and after read-back for all of
them; `altsyncram_soft` has 3 objects in memory and 0 after the `-netlist` read-back (it is lowered
to logic; nothing to count). Leg a is 100% everywhere; every leg-b and leg-d defect is the loss of
the location of a driver of an output port bit (D1/D3; ch_intrinsics also D2). Leg d records no
specificity (the placed names carry no location list; the leg-b figure applies).
Source: `prov-cover --legs a,b|c|d --smallest-first` reports (`tests/prov/expect_vtr14.txt` and
`expect_vtr14_d.txt` list the known defects).

## Decisions log

1. **Generalized flow** (Peter, 2026-10-19; was PHASE4 decision 5): "build the generalized flow
   in OdinIII to hit on this". The items serve D10 follow-up 2's goals; the experiments are
   designed after v1.0.
2. **Provenance coverage is a gate** (Peter, 2026-10-19; was PHASE4 decision 7): "4.19 using
   the Provenance path we should expect a very good report". Full coverage apart from named
   exception classes.
3. **Graph export is Yosys JSON plus Odin III keys** (Peter, 2026-10-19, PHASE5 decision 3):
   one format, one reader.
4. **Spec approved** (Peter, 2026-10-20): the Phase 6 spec v1.2 is approved, all section 9 questions
   decided (Q1-3, Q5-Q11 as recommended). Q4 rule: memories (hard RAM, any memory cell) treat their
   inputs as primary outputs and their outputs as primary inputs; black boxes and hard blocks that
   state a latch or register follow the same rule (registers in the netlist already cut alike); all
   other black boxes are combinational paths.
   `levelize` scope: just an algorithm to count the levels of logic from a combinational node
   perspective; timing detail can be added later (no delays, arcs or criticality). Amendment to
   decision 2: `inherited` and `pad` are approved named exception classes (strict figure always
   reported).
5. **`--auto-blackbox` infers port directions** (Peter, 2026-10-21; amends 2D EL-11, spec v4.32):
   every port of an implicit black box was `inout`, so an unknown cell wired to a driven net failed
   with "has two drivers", and reverse engineering reads netlists full of unknown library cells.
   Per cell type: a port that drives an otherwise undriven net bit is an output, every other port
   is an input, one WARNING per type lists the directions. The inference is order-independent
   (agent defaults inside the ruling, Peter may override): each instance connection votes per net
   bit (output: no known driver, a known reader, and the only unknown port on the bit; input: a
   known driver; inout: a module or cell `inout`; no vote: dangling or ambiguous bit, a constant
   casts none), and a port settles per type from all its votes: only output votes, output; only
   input votes or none, input; both kinds or an `inout` vote, `inout` with a WARNING note (no false
   two-driver error). There is no "first in source order" rule: a net with several unknown ports and
   no known driver casts no vote, and when no settled output drives it the WARNING names the net.
   Tie-break (agent default, 2026-10-21): every cell type should have an output, so on an ambiguous
   net the only port of an output-less type becomes that type's output (fixpoint; two output-less
   types on the net, or two candidate ports in a type, stay inputs; the WARNING says "chosen as
   output: the type had none").
   Two settled outputs on one net are reported in the WARNING naming both ports. `read_json
   --auto-blackbox` follows the same rule, with the same text, when no cell gives
   `port_directions`.
6. **Leg d starts VPR itself** (agent default, 2026-10-21; spec P-6 said through `tools/vtr-qor`):
   `tools/vtr-qor` runs with `-delete_result_files`, so a run keeps no `.net` or `.place`. The leg
   runs `vpr --pack --place` (channel width 100, seed 1, k6_frac arch) on leg b's provenance-named
   BLIF, in `tools/heavy-run`'s slice like every heavy job. Routing is not needed for a placement.
7. **Block name to netlist name** (agent default, 2026-10-21): a placed block or atom name is the
   BLIF net name verbatim (VPR names a primitive after its output net, a cluster after one of its
   atoms; evidence in the `vpr_leg.py` docstring). A provenance name is decoded as `read_blif
   --hdl-locs` does (5.4 L-5), then the net must exist in the read-back and its driver must
   have a location (inherited, D-5). The check includes the atoms inside each cluster from the
   `.net` file, so the gate covers "the atoms of every placed block".
8. **Pad rule for leg d** (agent default, 2026-10-21): a pad is an input pad (the port bit name) or
   `out:` plus an output port bit. A plain output port bit name is the logic atom that drives it
   and must resolve, so the D1/D3 losses show in leg d. `gnd`, `vcc`, `unconn` count as
   `const-table`.
