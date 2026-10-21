# Gate-level netlist fixtures (Phase 6.1)

Spec: `docs/specs/2026-10-21-6-analysis-flow-design.md` section 1 (G-1 to G-7).

- `cells/cmos.v`: Verilog models of `BUF`, `NOT`, `NAND2`, `NOR2`, `DFF`, `DFFSR` (the cell library,
  read with `read_verilog --lib` or as a `-v` file). `cells/cmos_map.v`: the Yosys techmap onto
  those cells. `cells/dffsr_sync.blif`: the synchronous BLIF model of `$_DFFSR_PPP_` that `gen.sh`
  appends to a `mapped.blif` that uses it (the IR has no bit-level asynchronous set/reset cell).
- `<micro>/rtl.v`: the RTL (a copy of `tests/micro/verilog/...`, or hand-written for `regen` and
  `cone`; `adder/rtl.v` instantiates `add_module`, which the source file defines but misnames).
- `<micro>/mapped.v`: the Yosys-mapped netlist, no attributes (the reverse-engineering case, no
  `src`). `<micro>/mapped_src.v`: the same with Yosys `src` attributes. `<micro>/mapped.blif`: the
  same netlist as BLIF (`.subckt $_NAND_` and so on).
- `unknown/unknown.v`: a netlist with a cell no library defines (G-7).
- `gen.sh`: regenerates all of it with Yosys 0.55 (`$VTR_ROOT/build/bin/yosys`): `synth -flatten`,
  `dfflegalize` to `$_DFF_P_` and `$_DFFSR_PPP_`, `abc -g NAND,NOR`, `opt_clean`,
  `insbuf -buf $_BUF_ A Y`, `techmap -map cells/cmos_map.v`, `check -assert`.

Tests: `tools/netlist-ingest` (CTest `netlist_cells_elab`, `netlist_ingest_<micro>_{verilog,blif}`,
`netlist_dump_<micro>`, `netlist_src_<micro>`, `netlist_unknown`).
