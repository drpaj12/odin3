# Odin III: Phase 4: VTR architectures, Altera via LPM, and the analysis flow

Spec: `docs/DESIGN.md` §6 step 7 (`read_arch`), §7 (partial mapping), §9 (writers), §12, §12.1,
§13 Q5; ADRs D10 (the scope; amends D5 and D6), D1, D3 (subprocess tools), D9 (provenance).
Inputs from Phase 3: the 3C `hardmap` rules (the fixed Parmys-default binding, PHASE3
decision 4), `lib/vtr.o3lib` (1G), the `tools/vtr-qor` harness and its `fcheck`
(functional check), `tools/hardmap-check`, `tools/equiv-check`. Oracle: VTR `3c9a4d23`
(`docs/ORACLES.md`). External trees are read-only.

**Goal (Peter, 2026-10-19): the tool works on VTR architectures and on Altera via LPM.** QoR
parity is no longer an exit criterion. Phase 4 also builds the generalized analysis flow
(4.15 to 4.19, decision 5): the inputs and outputs the post-v1.0 experiments need, not the
experiments.

**Exit test** (DESIGN §12 row 4, 4.13 and 4.14):

1. **Arch sweep (4.13).** The VTR designs (the Phase 3 set) on several VTR archs:
   `k6_frac_N10` (the Phase 3 arch), one DSP arch and one plain-LUT arch (which ones: open
   item 1). Pass = VPR routes (`vpr_status=success`), the `vtr-qor` fcheck passes, and the
   micros are equivalent (`equiv-check` against the Parmys golden for that arch).
2. **Altera via LPM (4.14).** The LPM netlist written by Odin III is equivalent to the RTL,
   using the LPM library's simulation models (`equiv-check` or `sim`). An optional Quartus Lite
   compile is Peter's hands, not CI.
3. **FSM and analysis flow (4.8, 4.15 to 4.19).** Each item's acceptance test passes; 4.19
   resolves every final-netlist object to the HDL apart from its named exception classes
   (decision 7).

Each sub-project gets its own spec (`docs/specs/`), plan and PRs. Model and effort follow
`docs/PHASE0.md` §9: mapping algorithms Opus `xhigh` to `high` with the advisor; importers,
writers and harnesses Opus `high`; bulk configs Sonnet. Peter reviews every mapping algorithm
(DESIGN §12).

## Checklist (kept items)

Each item has a one-line acceptance test.

- [ ] 4.1 VPR-XML import: `read_arch` reads a VPR arch's `<models>` and pb_types into a
  `.o3lib` tech library. Test: the imported library for the Phase 3 arch yields the same hard
  cells and ports as `lib/vtr.o3lib`; each sweep arch imports without error.
- [ ] 4.2 Generic hard blocks matched by exact port signature (Odin II style). Test: a
  design instantiating a model of an arch binds to that model; a port-signature mismatch is a
  located error or soft logic, as specified in the 4.2 spec.
- [ ] 4.3 Multiplier splitting generalised from the arch: split size and small-multiplier
  threshold from the XML; signed multipliers (light). Test: on the DSP arch the micros with
  `$mul` of several widths and signs are equivalent; on `k6_frac_N10` the output equals the 3C
  fixed k6 rules (which stay Parmys's).
- [ ] 4.4 Memory depth/width splitting to whatever RAM blocks the arch has. Test: memory
  micros are equivalent on every sweep arch; `k6_frac_N10` output equals the 3C k6 result.
- [ ] 4.5 Carry chains read from the arch model (light). Test: adder micros equivalent on the
  arch with a carry model; 3C maps to k6's `adder` only and stays unchanged.
- [ ] 4.6 FF feature matching: enable and sync/async reset mapped onto what the arch's FF
  supports, soft glue for the rest. Test: FF micros (enable, sync reset, async reset) are
  equivalent on an arch with a featureful FF and on a plain one.
- [ ] 4.7 Memory port merge: registered reads and writes merged into `$mem` ports; no
  `ram_style` attribute overrides. Test: a registered-read RAM micro yields one `$mem` with the
  read register merged, and is equivalent.
- [ ] 4.8 FSM detect and recode (restored, decision 6): a port of Yosys 0.55's `fsm` as its
  default script runs it (`fsm_detect`, `fsm_extract`, `fsm_opt`, `fsm_recode`, `fsm_map`;
  `-expand` and `-export` are not in v1.0). It runs where Parmys runs it, right after `proc`
  (Parmys: `proc -norom; fsm; opt; wreduce`, `parmys.cc`). Encoding follows Yosys: the
  `fsm_encoding` attribute (`none`, `one-hot`, `binary`, `auto`) is honoured, and `auto` is
  one-hot below 32 states and binary from 32; `-encoding one-hot` forces one-hot for every FSM.
  Provenance: the recoded state register and next-state logic get `DERIVED` records whose
  parents are the original state register and its logic, and each FSM gets attributes (state
  count, encoding) that 4.16 and 4.17 export. Test: FSM micros are equivalent to the RTL after
  `fsm`; detection and encoding match Yosys 0.55 `fsm` on the micros (same FSMs, state counts
  and encodings); the 19 VTR designs with `fsm` in the script still route and pass fcheck.
- [ ] 4.13 Arch-sweep exit test (above). Test: the exit test item 1.
- [ ] 4.14 Altera target via LPM:
  - an LPM tech library (`.o3lib`), a basic path of memory plus simple items (decision 3), with
    exactly: `lpm_add_sub`, `lpm_mult`, `lpm_compare`, and `altsyncram` (single-port and
    simple-dual-port). The output is family-neutral: a family option sets the device-family
    parameters (`intended_device_family`), default Cyclone V, the one tested; Stratix is
    reachable by the option and untested (decision 4). Flip-flops are plain DFFs in soft logic, not `lpm_ff` (agent
    default, decision 3). Everything else goes to soft logic as generic structural Verilog gates
    or assigns;
  - mapping word-level cells to them (partial map, as for VTR hard blocks);
  - a structural Verilog writer that instantiates LPM modules with their parameters;
  - the same library reads LPM-structured Verilog back with LPM instances as hard cells (the
    starting point of "going from Altera up").
  Test: the exit test item 2; and writer output re-read by the front end gives the same
  netlist (LPM instances kept as hard cells).
  Out of scope: VQM, EDIF, `stratixiv_*`/device atoms, Altera `bind`/`unbind` to atoms, Titan.
- [ ] 4.15 Gate-level netlist ingest: structural Verilog over a cell library (cells given as
  Verilog models through `read_verilog -v`/`-y`, kept as cells or flattened to IR primitives)
  and BLIF (`read_blif`), with hierarchy and names kept. Test: a micro mapped by Yosys to a
  small CMOS cell set (BUF, NOT, NAND2, NOR2, DFF, DFFSR) and the same micro as BLIF both read
  in and are equivalent to the RTL.
- [ ] 4.16 `write_dot --color-by <attr>` and provenance on node labels: nodes coloured by the
  value of a named attribute (a legend per value), the HDL `file:line` on each node label when
  provenance has one. Test: a micro with an attribute set on some cells gives the expected
  colours and labels; the dot parses (Graphviz `nop`).
- [ ] 4.17 Graph export for analysis tools: a flat, documented format (nodes with ID, cell
  type, module path, attributes, level, provenance; edges driver pin to sink pin) that networkx
  and PyTorch Geometric load without Odin III. Test: export of a micro reloads in Python with
  the same node and edge counts as the IR; the format is versioned.
- [ ] 4.18 `levelize` pass: the combinational level of each cell from inputs and register
  outputs, stored as an attribute; combinational loops reported. Test: levels on micros match a
  hand count; a loop gives a located warning.
- [ ] 4.19 Provenance coverage report: the share of final-netlist objects (BLIF after `abc`,
  LPM Verilog, VPR block names from the `.place` file) that resolve to an HDL source location.
  The path is the provenance chain: `DERIVED` records across every pass, including `abc`
  (PHASE3 decision 14), back to `SOURCE` spans (D9). Test (decision 7): on the Phase 3 design
  set and the LPM micros, every LUT, latch, hard block and LPM instance of the final netlist
  resolves to at least one HDL location, and every VPR placed block's atoms do. The only
  exceptions are named classes (constant drivers, buffers the writers add), each counted in
  the report. An unresolved object outside those classes is a bug. Recorded per design: the
  coverage, and the specificity (median and 90th percentile of distinct HDL lines per
  object).

## Dropped from Phase 4 (D10)

| Old item | Reason |
|---|---|
| 4.9 mux collapsing | Done: `opt` already does it (`opt_muxtree`, `opt_reduce`) |
| 4.10 matcher v1 | The matcher is not in v1.0 (D5 amended); port signature and fixed rules instead |
| 4.11 project mapping rules | Not needed to make the tool work; the project file still reads and stores them (DESIGN §4.0) |
| 4.12 per-bit `opt_dff` parity | Not needed for "the tool works"; QoR parity is no longer a goal |

Phases 5 (deferred) and 6 (removed) are recorded in DESIGN §12.1.

## Dependencies on Phase 3

- 3C `hardmap` and its rules are the base: 4.2 to 4.6 generalise them from the arch, and must
  reproduce the fixed k6 result on `k6_frac_N10`. 4's `partial_map` replaces `hardmap` (PHASE3
  sub-project 3C).
- `tools/vtr-qor` and its `fcheck` are the harness for 4.13; its caps and smallest-first order
  apply (heavy-run lock: sweeps run when lanes are idle).
- `lib/vtr.o3lib` and the `.o3lib` format (1G) are the target of 4.1 and the model for 4.14.
- `write_blif --vtr` (3E), `lower` (3B) and the linked `abc` (3D) are unchanged.
- The `equiv-check` models (PHASE0 #7/#8) and the simulator are the oracle for 4.14; LPM
  simulation models for the four LPM cells must be added to them.

## Decisions log

1. **v1.0 scope** (Peter, 2026-10-19): "since our goal is to make Odin III ... I just want the
   tool to work"; "go with all the recommendations (we will be looking reverse engineering from
   a different perspective of the goal of going from altera all the way up) I would like to
   target Altera devices with LPM and structured verilog, but I'm not interested in their
   low-level formats." Phase 4 is "the tool works on VTR architectures and on Altera via LPM";
   Phase 5 is delayed; Phase 6 is removed from v1.0; Phase 7 is kept without the EDIF writer.
   Recorded in `docs/ADR/D10.md` and DESIGN §12, §12.1.
2. **LPM device family** (Peter, 2026-10-19): "Cyclone V is fine, go ahead just a basic set of
   memory and simple items is fine - it's just a basic path". Cyclone V (Quartus Lite) is the
   device family; device-independent LPM where no device parameter is needed. This supersedes
   the earlier agent default and answers DESIGN §13 Q5 for v1.0.
3. **LPM set** (Peter, 2026-10-19, same message; the exact list is the coordinator's):
   `lpm_add_sub`, `lpm_mult`, `lpm_compare`, `altsyncram` (single- and simple-dual-port, Cyclone
   V). Flip-flops: plain DFF in soft logic rather than `lpm_ff` (agent default, Peter may
   override; it needs no extra library cell or simulation model). Everything else is soft logic
   as generic structural Verilog gates or assigns.

4. **Altera family** (Peter, 2026-10-19): "we just want a path to stratix (nothing deep) so an
   LPM family neutral is completely reasonable". The LPM output is family-neutral, with a
   family option (default Cyclone V, tested in Quartus Lite). Stratix is set by the option and
   untested; Quartus Lite has no Stratix support. Amends decision 2.
5. **The generalized analysis flow** (Peter, 2026-10-19): "yes to all the recommendations and
   build the generalized flow in OdinIII to hit on this". Items 4.15 to 4.19 serve the goals
   in D10 follow-up 2 (netlist reverse engineering, graph methods, provenance back to HDL).
   The experiments (Gate-Breaker dataset, Bayesian classifier, graphlets, graph sandwich, GNNs,
   the placement study) are designed after v1.0, as plugins over these outputs. Phase 3
   parity scope: PHASE3 decision 53.
6. **FSM restored** (Peter, 2026-10-19): "reinclude the FSM detection to map to one-hot
   ecoding for FPGAs". 4.8 is back in Phase 4 (it had been dropped by D10). Agent defaults:
   port Yosys `fsm` and run it right after `proc`, as Parmys does, so `--vtr-compat` matches
   Parmys; Yosys's `auto` rule (one-hot below 32 states) is the default, `-encoding one-hot`
   forces one-hot. The pass is a mapping algorithm: Peter reviews it (DESIGN §12).
7. **Provenance coverage target** (Peter, 2026-10-19): "4.19 using the Provenance path we
   should expect a very good report". 4.19 is a gate, not only a measurement: full coverage
   apart from named exception classes; an unexplained unresolved object is a bug.

## Open items

1. **Sweep archs.** Which DSP arch and which plain-LUT arch from `$VTR_ROOT/vtr_flow/arch` join
   `k6_frac_N10_frac_chain_mem32K_40nm.xml`; each needs a Parmys golden for the micro
   equivalence check.
