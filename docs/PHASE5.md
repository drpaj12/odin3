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

- [ ] 5.1 Altera via LPM, light (code on `feat/5.1-lpm-spec`; tick when the PR merges; PHASE5 decisions 7 and 8) (was PHASE4 4.14; decision 1):
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
  Spec `docs/specs/2026-10-20-5.3-read-json-design.md`, plan `docs/plans/2026-10-20-5.3-read-json-plan.md`
  (PRs 5.3a parser, 5.3b reader and pass, 5.3c round-trip tool and Yosys fixtures; decision 10). The
  fixpoint is generation 2 to 3 (spec R-1); `netlist-compare` is identical where covers survive the
  `$sop` to `$lut` write (spec R-3, question 3).
- [ ] 5.4 HDL locations survive write and read:
  - BLIF: `read_blif` decodes `--name-style provenance` names (`hier/<name>@file:line`) back
    into provenance records pointing at the HDL, beside the IMPORTED record of the BLIF line;
  - Verilog: the writer adds `(* src = "file:line" *)` where Verilog allows an attribute
    (declarations, instances, `always`), keeping the `// file:line` comment on assigns;
    `read_verilog` turns `src` attributes into provenance records;
  - JSON: `read_json` turns `"src"` attributes into provenance records.
  Test: for the micros, every object that had an HDL location before writing has the same
  location after reading, in each of the three formats.
  Spec `docs/specs/2026-10-20-5.4-hdl-locations-design.md`, plan
  `docs/plans/2026-10-20-5.4-hdl-locations-plan.md` (PRs 5.4a text, helper and writers; 5.4b JSON,
  5.4c Verilog `-netlist`, 5.4d BLIF `--hdl-locs`, in three parallel lanes; 5.4e `tools/loc-check` and
  the numbers; decision 11). "Same location" is per object kind as the spec's section 5 table says,
  with named exception classes X-1 to X-9, each counted.

## Dependencies

- 5.1 needs only the `.o3lib` format and the 3C rule style; it does not wait for Phase 4's
  generic port-signature matching (4.2). It can start when a Phase 3 lane frees up (decision 4).
- 5.3 is the reader for Phase 6's graph export (6.3), which is this JSON plus Odin III keys.
- 5.4 feeds Phase 6's provenance coverage gate (6.5) on read-back netlists.
- Task order (specs of 2026-10-20): 5.3a (parser) and 5.4a (location text, helper, writers) run in
  parallel and touch disjoint files (5.4a touches the JSON writer's `src`, 5.3 only if its R-5 check
  finds a defect); then 5.3b; then 5.3c, 5.4b, 5.4c and 5.4d (5.4b needs 5.3b, 5.4c and 5.4d only
  5.4a); 5.4e last. At most three lanes at once.

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
   - `write_blif` refused LPM cells at first, so the BLIF leg of spec T-3 was dropped; decision 8 brings
     it back;
   - `iverilog -Wall` notes every open instance input as a floating port; the lint leg filters
     exactly those notes (Altera defaults apply, spec H-4) and fails on any other finding, and
     Verilator runs with `-Wno-PINCONNECTEMPTY -Wno-PINMISSING`;
   - Verilator is a local-only extra leg of the lint (`lpm-check` runs it when installed); CI has iverilog
     only and Verilator is not added to CI (review M3, Peter's rule);
   - review of 47a5ed88: H1, M1, M2, M3, L1, L4, L5, L7 fixed; L2, L6 accepted with the reasons in spec
     §17; L3 superseded by decision 8;
   - the Debug `check` logs rule 11 warnings (unconnected pin) for the open LPM inputs; they are
     warnings, not failures.
8. **BLIF carries LPM parameters** (agent default, 2026-10-19; Peter asked why LPM cannot be a black
   box in BLIF; he may override). `write_blif` (PLAIN dialect) writes, after the `.cname` of a
   `.subckt` of a tech-library cell, a Yosys-style `.param NAME VALUE` line for each parameter the
   reader cannot derive (format of Yosys `backends/blif/blif.cc` `dump_params`: strings quoted with
   `\\` and `\"` escapes, integers as 32 binary digits, bits as `01xz`); derivable and default
   parameters write nothing, so existing BLIF is byte-identical. `read_blif` sets a `.param` on a
   library cell that declares it (the cell is rebuilt on the same nets, name and attributes);
   elsewhere it stays an attribute. The VTR dialect still refuses parameterised `.subckt` cells.
   `lpm-check` has the BLIF leg (write, read, write identical, `netlist-compare` identical with
   `.blackbox` stubs from the `.o3lib`). Docs: 1C amendment, 3E note, `writer.h`, `reader.h`,
   `attrs.h`. The IR is not changed.
9. **5.2 `read_verilog -icells` implemented** (agent defaults, 2026-10-20; Peter may override; spec
   `docs/specs/2026-10-20-5.2-icells-design.md` v1.1 section 12 has the eleven questions with
   recommendations, section 13 what the implementation changed; plan
   `docs/plans/2026-10-20-5.2-icells-plan.md`). No IR, ABI, ADR or Verilog-without-`-icells` change.
   - the option is `-icells` and `--icells`; an instance of a `$` name is that registered cell (item 0
     of EL-6, before the module chain, so no stub or module shadows it; an info line when one exists);
     every registered type but module, port and process may be instantiated, not only the ones the
     writer emits as instances; an unregistered `$` name is an error, also with `--auto-blackbox`;
     refused with `--lib` and `--parse-only`;
   - widths are strict for these instances (an error, not the resize warning), parameter kinds are
     checked, a string given to a BITS parameter is its bytes and `""` the empty value (the writer's
     spelling), and the type's `verify` failure is located on the instance;
   - the round trip is `tools/icells-check`; what each level proves (review H1): `-icells` is exercised
     where a `$` instance exists in the written Verilog, i.e. the word and LPM levels and the `inst`
     level (hand-written netlists, one per cell type the writer emits as an instance: 16 types, each
     required by file name). At the gate and LUT levels no `$` instance exists (every cell is an assign
     or an always block), so they prove the plain Verilog read-back of the writer's output (equivalent
     by `equiv-check` on BLIF), also a 5.2 requirement, not `-icells`. Equivalence: the RTLIL Checker
     (word, LPM, inst) or `equiv-check` (gate, LUT); fixpoint: cell census plus a per-cell signature
     (type, parameters, widths, wiring up to renaming); instances: type, name, parameters, pins; the
     report counts the `$` instances read via `-icells` and the library instances;
   - tests: `test_icells` (21 cases), `test_icells_roundtrip` (16 instance types), `python_tools` (the
     tool's unit tests and an end-to-end mutated read-back per leg), CTests `icells_inst`,
     `icells_insttext`, `icells_cells_word/lpm`, `icells_lpm_cells/soft`, `icells_plain_gate/lut`
     (default set, about 40 s) and `icells_micros_<level>` over the 68 micros (label `sweep`, run with
     `ctest --test-dir build/debug -R icells_micros`; nothing in heavy-run). CI has no Yosys 0.55, so
     the CTests that need it are local-only; CI runs `test_icells`, `test_icells_roundtrip` and
     `python_tools` (its end-to-end tests skip without Yosys 0.55);
   - `-icells` also reads a `$sop` COVER parameter from the writer's quoted rows, and a user module
     named `$x` that is not a registered type still resolves as a module;
   - documented exceptions, none silent (spec X-9 to X-13, `tests/icells/expect.txt`): `always`
     storage has no text fixpoint; `multiply_hard_block.v` at gate/LUT (the read-back of the written stub is refused by `write_blif --vtr`; passes without the stub; cause not confirmed); `$pow` is
     unproven by Yosys and `lower` cannot take it; `$memrd`/`$memwr` have no RTLIL form (text legs only);
   - `lpm-check` keeps `noroundtrip altsyncram_soft.v` (the text fixpoint of X-9); `icells-check`
     covers that micro at the LPM level.
10. **5.3 spec and plan** (Peter approved all ten recommendations, 2026-10-20; spec
    `docs/specs/2026-10-20-5.3-read-json-design.md` section 12 has the ten questions with
    recommendations; plan `docs/plans/2026-10-20-5.3-read-json-plan.md`). No IR, ABI or ADR change. In
    short: `read_json [--top] [--auto-blackbox]`, an iterative parser with limits (no recursion), a DOM
    and two walks, nets created by ascending bit number so the write, read, write fixpoint is generation
    2 to 3, unregistered Yosys cell types are a located error (an implicit black box with a warning only
    under `--auto-blackbox`), no resize of widths, `$sop` is written as `$lut` so gen1 and gen2 are
    equivalent and not always `netlist-compare` identical, no public ABI entry, no `$mem_v2`.
    Implemented as 5.3a/b/c (spec v1.1, plan deviations 1 to 20; agent defaults, Peter may override):
    `tools/json-check` over `tests/json/{word,gate,lut}.script`; measured in Debug, all 68 micros read back
    equivalent at the word, gate and LUT levels, R-1 holds on all, gate and LUT are `netlist-compare`
    identical; the Yosys 0.55 fixtures (`tests/json/yosys`, 182 files) read and fixpoint in CI (CTests
    `json_yosys_<level>`), 7, 9 and 10 of them are located errors by the M-5/M-6 rules listed in
    `tests/json/expect.txt`; the 14 small VTR designs at the LUT level (`json_vtr14_lut`): 10 equivalent,
    4 unproven by `equiv-check` (several clocks). R-5 (instance parameters written as JSON parameters) is a
    separate small PR.
11. **5.4 spec and plan** (Peter approved all ten recommendations, 2026-10-20, including
    question 1, the IR-13 amendment; spec
    `docs/specs/2026-10-20-5.4-hdl-locations-design.md` section 12 has the ten questions with
    recommendations; plan `docs/plans/2026-10-20-5.4-hdl-locations-plan.md`). **Question 1 (approved by
    Peter 2026-10-20; amend IR-13 in `docs/IR.md` in Task 3):** readers restore a location as one `DERIVED` record (parents:
    the restored HDL `SOURCE`, then the reader's own `IMPORTED`), a one-sentence amendment of IR-13;
    fallback is one `SOURCE` record. In short: writers print only `SOURCE` leaves (an `IMPORTED` line
    never comes back as HDL), `src` in Yosys form with end span and up to 8 locations, Verilog `(* src *)`
    plus the unchanged `// file:line` on assigns, plain BLIF `.attr src` under a new `--src-attr`,
    readers decode only under `read_verilog -netlist`, `read_blif --hdl-locs`, `read_json` (default,
    `--keep-src-attr` to keep the string); `tools/loc-check` reports same, line, inherited, lost by class
    and gained per format, level and object kind, the baseline for 6.5.
