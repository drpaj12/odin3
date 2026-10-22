# Gate mapping: ABC optimisation to an `.o3lib` gate library (design)

Status: approved in brainstorming with Peter, 2026-10-21 (decisions G-1 to G-9); this file is for his
review. Post-v1.0 core feature (the first after the v1.0.0 tag).

## 1. Goal

Today the `abc` pass always ends in LUTs (`$lut`). Add a second ending: map the optimised logic onto
the gate cells of an `.o3lib` library, so the design stays in Odin III's provenance IR and
`write_verilog` writes structural gate-level Verilog in which every gate traces to HDL text.

Main user: Loki's L0 ground-truth datasets in the style of Gate-Breaker (Faruque, Jamieson, Patooghy,
Badawy, IPCCC 2025: Yosys + ABC mapped to a six-cell CMOS library), built from the benchmarks with
provenance as the answer key.

**Done when:** the micros run `read_verilog; proc; opt; lower; read_techlib L; abc --gates L;
dfflibmap L; write_verilog` for `L` = `lib/generic_gates.o3lib` and a six-cell library
(`lib/cmos6.o3lib`: BUF, INV, NAND2, NOR2, DFF, DFFSR), and every result is formally equivalent to the
RTL (`equiv-check`), with only library cells, constants and wires left, and every gate carrying
provenance.

## 2. Decisions

| # | Decision |
|---|---|
| G-1 | Libraries are `.o3lib` only (Peter: any library can be written in that format). A Liberty importer is out of scope. |
| G-2 | Gate provenance is per cone (the existing `coarse_records`): the AIG loses structure, so the link is less certain but kept. `--prov fine` with gates falls back to cone with a NOTE. |
| G-3 | Mappers: `map` (ntk space) and `&nf` (gia space). `amap` is not accepted (needs a second library form). |
| G-4 | `abc --gates <library>` selects gate mapping; the library is a `library` name already read with `read_techlib`. Odin III derives ABC's genlib text and loads it in memory (`Mio_LibraryRead` with a buffer); scripts still never read files (`read_genlib`, `read_lib` stay refused). |
| G-5 | Default gate script `lib/abc/gates.abc`: the optimisation steps of `lib/abc/vtr.abc` then `map -a`. A user `--script` with `--gates` must end gate-mapped (`map` or `&nf`). |
| G-6 | Flip-flops and latches map to the library's state cells in a new pass `dfflibmap <library>` (match on edge or level and init); no match is an error. After `lower` only `$_DFF_P_`, `$_DFF_N_` (with `lower --neg-edge keep`), `$_DLATCH_P_` and `$_DLATCH_N_` exist: asynchronous controls are made synchronous (`--async sync`, the default) or refused (`--async error`), and `$_DFFSR_` is not registered in v1.0. So library cells with asynchronous set/reset (e.g. DFFSR) are never chosen; supporting them waits for `$_DFFSR_`. |
| G-7 | Constants and feed-throughs use `TIE0`, `TIE1`, `BUF` when the library has them (cells whose `fn` is constant 0, constant 1, or identity); otherwise `write_verilog` writes `1'b0`/`1'b1` and a continuous assign. |
| G-8 | Verification: `write_blif --gates-as-names` writes each library gate as a `.names` cover of its `fn`, so `tools/equiv-check` compares the gate netlist with the RTL's BLIF. |
| G-9 | Treated as a mapping algorithm: Opus for the spec and the core changes; Peter reviews the PR. |

## 3. The genlib derived from a library

For each cell of the named library with kind `gate`, exactly one 1-bit output with an `fn`, 1 to
`ODIN3_ABC_GATE_MAX_INPUTS` (6) 1-bit inputs, and no parameters:

- **Formula:** the irredundant sum of products of the cell's truth table (the truth table comes from
  the cell's `fn` as `gate_truth` in `src/passes/abc/region.c` already computes it; the ISOP is ABC's
  `Kit_TruthIsop`, computed in the adapter), printed in genlib syntax (`*`, `+`, `!`, `CONST0`,
  `CONST1`).
- **Area:** the cell's `area` text as a number (a non-numeric or missing area is an error naming the
  cell); **delay:** the `delay` text when numeric, else 1, the same for every pin (`PIN * UNKNOWN 1 999
  <delay> 0 <delay> 0`).
- Cells that do not qualify are skipped with one NOTE each; a library with no usable gate, or without
  a gate set ABC can map an AIG to (ABC needs at least an inverter and a 2-input function from which
  AND can be built), is an error before ABC starts.
- The text is deterministic (cells in library order) and its SHA-256 goes into the pass log, so a run
  can be traced to the exact library.

## 4. The pass flow with `--gates`

1. **Regions** as today (`region.c`): one run per clock domain per module; in-domain flip-flops enter
   ABC as registers; other domains' flip-flops, latches, hard cells, black boxes, tristates and
   instances are cut points. Library gate cells already in the design enter ABC as their functions
   (`gate_eligible`), so a netlist can be re-mapped.
2. **Engine:** before the script, the adapter loads the genlib (`Mio_LibraryRead(NULL, text, NULL, 0,
   0)` then `Mio_UpdateGenlib`), then runs the script as today.
3. **Result:** a new engine call reads the mapped network: for `map`, the mapped `Abc_Ntk` (a
   `Mio_Gate` per node); for `&nf`, the gia cell mapping converted to a mapped `Abc_Ntk` with ABC's
   own conversion, so one reader serves both. It returns, per node, the gate name, its fanins (other
   nodes, region inputs or register outputs) and constant outputs.
4. **Splice:** each node becomes an instance of the library cell of that name (`odin3_celltype_find`,
   `odin3_node_create_connected`, as `partial_map` and `lpmmap` do), pins wired in genlib pin order
   (the cell's input order); registers are recreated as today; constants and feed-throughs per G-7.
5. **Provenance:** cone records (G-2) for every new gate.

The engine ABI gains the library load and the cell-result read; `ODIN3_ABI_VERSION` goes from 4 to 5
(no stability promise, release notes say so).

## 5. Script checker

- New network state `ABC_CELLMAPPED` (both spaces). `map` (ntk, needs a strashed network) and `&nf`
  (gia, needs an unmapped gia, like `&if`) produce it; their option letters get the same per-letter
  range and crash checks as the existing rows (`allow.c`).
- With `--gates`, the script must end `ABC_CELLMAPPED`; without `--gates`, `map` and `&nf` are refused
  (no library is loaded). `if`/`&if` endings with `--gates` are refused (LUTs are not gates).
- The end message names both endings.

## 6. `dfflibmap <library>`

- For each `$_DFF_P_`, `$_DFF_N_`, `$_DLATCH_P_` and `$_DLATCH_N_`: pick the library state cell
  whose `seq` has the same edge (or level), no asynchronous control, and an init that is `x` or equal
  to the cell's init; on several matches, the smallest area. A `$_DFF_N_` with no negedge library
  cell uses the posedge cell with a library inverter on the clock (as `lower --neg-edge invert`
  would). Any other state cell type is an error naming it.
- No match: error naming the cell type and the library.
- Provenance: the new cell takes the old cell's records unchanged (one-to-one).

## 7. Writers

- `write_verilog`: library cells are written by type and pin name (`write_instance` already does).
  Constants and assigns per G-7.
- `write_blif --gates-as-names`: each library gate as `.names` from its truth table; state cells as
  `.latch` (edge from `seq`); without the option, library cells stay `.subckt` (as today).

## 8. Libraries

- `lib/generic_gates.o3lib` (exists).
- New `lib/cmos6.o3lib`: BUF, INV, NAND2, NOR2 (gates, area 6, 3, 4, 4, as in Yosys's `cmos_cells.lib`), DFF (posedge, area 18), DFFSR
  (area 18; posedge, active-high asynchronous set and reset, S wins over R, as Yosys's `cmos_cells.lib`, so the library matches
  Gate-Breaker's; `dfflibmap` does not choose it in this version, G-6), written by hand from the
  function each cell computes.

## 9. Tests

- Unit: genlib text from `generic_gates` and `cmos6` (golden text); the qualifying rules and errors;
  the script checker rows and endings; the cell-result reader on a small mapped network; `dfflibmap`
  matching, area tie-break, no-match error.
- Micros: every `tests/micro` design through the flow of section 1 with both libraries; `equiv-check`
  against the RTL BLIF passes; the written Verilog contains only library cells, constants and assigns;
  every gate has a provenance record.
- `abc --gates` without `read_techlib`, with an unknown library, with a LUT-ending script: errors.
- A re-map: a gate netlist read back (`read_techlib; read_verilog`) and mapped again stays equivalent.

## 10. Not in scope

Liberty import; `amap`; fine (per-gate) provenance; asynchronous set/reset flip-flops (`$_DFFSR_`); timing-driven mapping beyond unit or given delays;
multi-output cells; gate sizing; placement or anything after the netlist.
