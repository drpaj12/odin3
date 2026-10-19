# Odin III: Phase 5: outputs and read-back

Spec: `docs/DESIGN.md` §9 (writers), §4 (readers), §12; ADRs D11 (this phase), D10 (scope,
LPM), D9 (provenance). Inputs: the writers of Phase 1 (1C BLIF, 1F Verilog, JSON, dot, RTLIL)
and Phase 3 (3E `write_blif --vtr`, `--name-style provenance`), the readers `read_blif` (1C) and
`read_verilog` (2C), the `.o3lib` format (1G) and the 3C `hardmap` rule style. External trees
are read-only.

**Goal (Peter, 2026-10-19).** Odin III's outputs are BLIF, JSON and Verilog, at different
levels (word, gate, LUT, LPM). Each can be read back by Odin III with the link to the HDL kept.
Peter: "Phase 5, I would argue is just an output path that's pretty simple since it's just
outputting Verilog ... the LPM outpath I don't think we need to test it heavily until I need
that step so it's a simpler output form ... for each of these outputs we need to be able to
read it." Read-back is required for BLIF, Verilog and JSON; dot is for viewing and RTLIL is
for Yosys only (decision 2).

**Exit test** (DESIGN §12 row 5): items 5.1 to 5.4 pass their tests. Every write/read round
trip in 5.2 to 5.4 is equivalent (`equiv-check`) and keeps the HDL locations.

## Checklist

- [x] 5.1 Altera via LPM, light (code on `feat/5.1-lpm-spec`, awaiting review; PHASE5 decision 7) (was PHASE4 4.14; decision 1):
  - an LPM tech library (`.o3lib`) with exactly `lpm_add_sub`, `lpm_mult`, `lpm_compare` and
    `altsyncram` (single-port and simple-dual-port). Flip-flops are plain DFFs in soft logic,
    not `lpm_ff`. Everything else is soft logic as structural Verilog gates or assigns;
  - fixed mapping rules in the 3C `hardmap` style (`$add`/`$sub` to `lpm_add_sub`, `$mul` to
    `lpm_mult`, comparisons to `lpm_compare`, `$mem` to `altsyncram`), with LPM parameter
    translation (`LPM_WIDTH`, `LPM_DIRECTION`, ...);
  - the output is family-neutral: a family option sets `intended_device_family`, default
    Cyclone V; Stratix is reachable by the option and untested (Quartus Lite has no Stratix);
  - written by the existing Verilog writer (hard cells are module instances with parameters);
  - read back by `read_verilog` with the LPM library: LPM instances are hard cells.
  Test: one micro per LPM cell writes, reads back to the same netlist, and passes a Verilog
  lint (Icarus or Verilator with stub modules). Equivalence under LPM simulation models and a
  Quartus compile wait until Peter needs that step.
- [ ] 5.2 Verilog read-back at every level: a `read_verilog -icells` mode (as Yosys's) that
  reads instances of Odin III's own cell types (`$mem*`, `$shiftx`, `$pow`, `$dffsr`,
  `$dlatch`, ...) as those cells, not as unknown modules. Test: for the micros, write Verilog
  after `opt` (word), after `lower` (gate), after `abc` (LUT) and after 5.1 (LPM); read each
  back; each is equivalent to the netlist written.
- [ ] 5.3 Yosys JSON reader (`read_json`, was 7.2): reads the Yosys JSON netlist schema that
  `write_json` writes, and Yosys's own JSON. Test: `write_json` then `read_json` round trip is
  identical under `netlist-compare` on the micros and the BLIF goldens; a Yosys-written JSON of
  a micro reads and is equivalent to its RTL.
- [ ] 5.4 HDL locations survive write and read:
  - BLIF: `read_blif` decodes `--name-style provenance` names (`hier/<name>@file:line`) back
    into provenance records pointing at the HDL, beside the IMPORTED record of the BLIF line;
  - Verilog: the writer adds `(* src = "file:line" *)` where Verilog allows an attribute
    (declarations, instances, `always`), keeping the `// file:line` comment on assigns;
    `read_verilog` turns `src` attributes into provenance records;
  - JSON: `read_json` turns `"src"` attributes into provenance records.
  Test: for the micros, every object that had an HDL location before writing has the same
  location after reading, in each of the three formats.

## Dependencies

- 5.1 needs only the `.o3lib` format and the 3C rule style; it does not wait for Phase 4's
  generic port-signature matching (4.2). It can start when a Phase 3 lane frees up (decision 4).
- 5.3 is the reader for Phase 6's graph export (6.3), which is this JSON plus Odin III keys.
- 5.4 feeds Phase 6's provenance coverage gate (6.5) on read-back netlists.

## Decisions log

1. **LPM is a light output path** (Peter, 2026-10-19; was PHASE4 decisions 2 to 4). LPM set:
   `lpm_add_sub`, `lpm_mult`, `lpm_compare`, `altsyncram`; flip-flops as plain DFFs (agent
   default). Family-neutral, default Cyclone V, Stratix by option and untested. Tested by write,
   read back and lint, not by equivalence under LPM models.
2. **Read-back scope** (Peter, 2026-10-19, "yes to all recommendations"): BLIF, Verilog and
   JSON must read back with HDL locations; dot (view only) and RTLIL (Yosys interop) need no
   reader. Yosys's own output comes in through JSON.
3. **One JSON format** (same message): Phase 6's graph export is Yosys JSON plus Odin III keys,
   so 5.3 reads both.
4. **LPM first** (same message): 5.1 starts as soon as a Phase 3 lane frees up, ahead of the
   rest of Phase 4.
5. **5.1 spec agent defaults** (agent default, 2026-10-19; Peter may override; spec
   `docs/specs/2026-10-19-5.1-lpm-design.md` §14 has the ten questions with recommendations):
   a new pass `lpmmap` with its own kit, not an LPM rule set inside `hardmap`; the four LPM
   cells are `blackbox` kind in `lib/lpm.o3lib` (no simulation functions until the deferred
   equivalence step); `.o3lib` gains `param NAME string "text"` (the LPM parameters are
   strings); the round trip is judged by a write, read, write Verilog fixpoint plus `check`,
   because the BLIF writer cannot carry string parameters and `netlist-compare` reads BLIF;
   every `$mul` and comparison maps (no width threshold); `$mem` maps only with one write and one
   read on one rising-edge clock, no `INIT`, no byte enable (else soft); the LPM names are
   verified against Intel documentation before any rule is coded (no local source exists for
   `lpm_compare`, nor for `lpm_add_sub` and `lpm_mult` beyond a commented-out use in `spree.v`).
6. **5.1 spec approved** (Peter, 2026-10-19): "yes to all recommendations" on the ten open
   questions of `docs/specs/2026-10-19-5.1-lpm-design.md` §14, which includes the rule IDs and the
   mapping rules as written. Chosen: (1) the four LPM cells are `blackbox`; (2) `.o3lib` gains
   `param NAME string "text"`; (3) the round trip is a write, read, write Verilog fixpoint plus
   `check` (BLIF as a bonus leg if it works); (4) a new pass `lpmmap`, not a `hardmap` rule set;
   (5) no `write_verilog --no-stubs` until Quartus is needed; (6) the minimal parameter set, and
   no `intended_device_family` on the `lpm_*` cells; (7) every `$mul` maps; (8) every comparison
   maps; (9) reuse the `hardmap` FF merge through a header if it splits cleanly, else data merge
   only; (10) verify the names against public Intel documentation. Task 1 then verified the
   names (spec §11, from Altera's UG-01063 and Quartus help). It changed two rules: R-3 maps a
   `RD_EN` only when it is constant 1 (the behaviour of `rden` low is not verified, V-7), and
   R-4 sets `address_reg_b` `"CLOCK0"` and `outdata_reg_b` `"UNREGISTERED"` as the Quartus wizard
   does. Still open: V-7 and a primary-document spelling of `"Cyclone V"`.
7. **5.1 implemented** (agent defaults under decision 6, 2026-10-19; Peter may override):
   - the pass is `lpmmap`, `lib/lpm.o3lib`, `tools/lpm-check`, micros in `tests/lpm/` (plan Tasks 2 to
     9); no heavy run was needed;
   - the `.o3lib` reader also lets a port be named `clock` (the modifier is read only after the
     width); `signed` and `x` stay reserved;
   - R-1 reuses hardmap's R-8 merge: `mem_load` and `mem_model_free` are exported from
     `src/passes/hardmap/mem.c` and declared in `mem.h`, with no change of behaviour (question 9,
     option i);
   - the round trip is a fixpoint of generation 2 to 3, not generation 1 to 2: reading turns an open
     LPM pin into the constant x (inputs) or a fresh net (outputs), so generation 2 differs from
     generation 1 in exactly those connections. `lpm-check` also compares instance by instance
     (type, name, parameters, connected pins; open, x and fresh nets count as equal);
   - `write_blif` refuses a design with LPM cells (`parameters neither declared nor implied by the
     connected formals have no BLIF form`), so the BLIF leg of spec T-3 is dropped;
   - `iverilog -Wall` notes every open instance input as a floating port; the lint leg filters
     exactly those notes (Altera defaults apply, spec H-4) and fails on any other finding, and
     Verilator runs with `-Wno-PINCONNECTEMPTY -Wno-PINMISSING`;
   - the Debug `check` logs rule 11 warnings (unconnected pin) for the open LPM inputs; they are
     warnings, not failures.
