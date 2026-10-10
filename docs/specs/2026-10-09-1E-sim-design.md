# 1E — Netlist simulator; "sim matches ABC on goldens"

Status: agent-approved under PHASE1 #12 (continue without stopping); exit-test definition from
PHASE1 #13. Phase 1, sub-project 1E. Spec §10 (`simulate`), ADR D4.

## Goal

A cycle-based, event-free simulator over the netlist view whose cell semantics come from the
op registry (the `simulate` hook IR-8 reserved), plus the Phase 1 exit test (PHASE1 #13): random
input vectors, our outputs compared cycle by cycle with an independent reference simulation of
the same BLIF.

Success: every `ok` golden without RAMs simulates; for each, 64 cycles × 3 seeds match the
reference on every primary output; CI runs the comparison on the committed BLIF fixtures.

## Semantics

- **Values:** 2-state per bit in Phase 1 (0/1). Latch `INIT` 0/1 is honoured; `INIT` 2/3
  (don't care / unknown) starts at 0, matching `tools/equiv-check` (PHASE0 #7) and the reference
  testbench, which initializes every register to 0. 4-state (x/z) is out of scope until a pass
  needs it.
- **Cycle:** one cycle = drive the non-clock primary inputs, settle combinational logic, apply
  the rising edge of every clock (update `$_DFF_P_` and `$_FF_`), settle, apply the falling edge
  (update `$_DFF_N_`), settle, sample primary outputs. Level latches (`$_DLATCH_P_/N_`) are
  transparent while enabled and settle with the combinational logic. Clock nets are the nets
  driving a latch's clock pin; they must be primary inputs (else the design is rejected with a
  message) and the simulator toggles them, not the random driver.
- **Combinational order:** the flat netlist is levelized once (iterative topological sort over
  driver → reader edges, latches cut the graph); a combinational loop is an error naming one net
  of the loop.
- **Hierarchy:** module instances are expanded into the flat simulation program at build time
  (iterative, no recursion); black boxes without semantics are an error ("cannot simulate
  `<type>`").
- **Cell semantics** come from each cell type's `simulate` hook, added now for: the bit gates
  (`$_BUF_ … $_MUX_`), constants, `$sop` (cover evaluation: ON-set or OFF-set), the latch family,
  and the word cells `$and $or $xor $not $add $sub $mul $eq $ne $lt $le $gt $ge $mux $pmux
  $reduce_*` (bit-blasted evaluation, unsigned/signed per parameters). Hard cells from a tech
  library (1G: `adder`, `multiply`) simulate by interpreting their `fn` expression ASTs with the
  techlib expression evaluator over the cell's port values (word arithmetic up to 64 bits per
  operand in Phase 1; wider operands fall back to bit-serial big-integer helpers for `+`, `*`).

## API

`src/sim/sim.{h,c}`: `odin3_sim_build(design, module, &sim)` (flatten + levelize),
`odin3_sim_set_input(sim, port bit, value)`, `odin3_sim_cycle(sim)`,
`odin3_sim_get_output(sim, port bit)`, `odin3_sim_destroy(sim)`; a driver
`tools/sim-check/odin3-sim-vectors in.blif --seed S --cycles N [--techlib lib.o3lib]` that prints
one line per cycle (`cycle inputs outputs` in port order, binary), deterministic for a seed.

## Reference and comparison (PHASE1 #13)

- **Pure-logic goldens** (no `.subckt` of a black box): ABC (`read_blif; write_verilog`) →
  Verilog; a generated testbench drives the same vectors (read from our driver's output) and
  prints outputs per cycle; Icarus (`iverilog`/`vvp`) runs it.
- **Goldens with `adder`/`multiply` instances** (ABC cannot read black-box `.subckt`s): Yosys
  (`external/yosys/build/yosys`: `read_blif -wideports`; `write_verilog -noattr`) keeps them as
  instances; behavioural Verilog models of `adder` and `multiply` (from the 1G library
  definitions, hand-checked) are appended; Icarus as above. This is an agent ruling extending
  PHASE1 #13 (ABC alone cannot reference these goldens); the reference stays independent of our
  simulator. As built (Task 5): Yosys also references goldens with falling-edge registers or a
  data input named `clock` (ABC clocks every register on one rising `clock`), reads with `-sop`
  (covers wider than 12 inputs) and runs `simplemap t:$sop` (`models/sop.v` for Yosys < 0.45);
  the reference's copy of the BLIF has latch init 2/3/absent set to 0. Results: `docs/PHASE1.md`.
- **Excluded:** goldens with RAM instances (206; Phase 4) and with implicit black boxes (`$pow`,
  `$_DFFSR_PPP_`), listed explicitly; the four multi-driver Odin II goldens (PHASE1 #15) if the
  reference refuses them.
- `tools/sim-check/sim-check` (bash): per golden, run our driver and the reference for seeds
  1..3, 64 cycles, compare line by line; a table and summary; exit 1 on any mismatch; `-j` bounded
  by the WSL memory rule. CTest `sim_check_fixtures` on `tests/golden/blif`.

## Files

`src/sim/{sim.h,sim.c,build.c,levelize.c,eval.c}`, `src/ir/cells/*` (simulate hooks),
`src/techlib/` (fn interpretation helper), `tools/sim-check/{odin3-sim-vectors.c,sim-check,models/adder.v,models/multiply.v}`, `tests/unit/test_sim_*.c`.

## Out of scope

4-state simulation, memories, VCD output (spec §10 lists it; added when a user needs waveforms),
event-driven or multi-clock-domain timing beyond the rising/falling-edge cycle above.
