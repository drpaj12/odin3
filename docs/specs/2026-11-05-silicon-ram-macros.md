# Silicon flow and RAM: keeping memories as macros (design note)

Status: note, 2026-11-05. Brief: controller briefs/odin3-opencores-fixes.md item 6. No code in this PR: the
installed PDK has no RAM macros (section 2), so the implementation is "too big for now" until a macro library is
installed and decided on (section 6).

Purpose (hub purpose hierarchy): Leman Russ III and the silicon provenance flow (v1.4 spec,
`2026-11-03-v1.4-silicon-provenance.md`) need real designs through Odin III -> OpenROAD -> GDS. Several
OpenCores and Koios-style cores keep their state in a memory (darkriscv register file and cache, reed_solomon
buffers). Measure: area (cell count and die area) of those designs, and that every placed cell keeps its HDL chain.

## 1. What happens today

`tools/silicon/silicon_flow.py` `odin_script` runs
`read_verilog --nolatches; hierarchy; proc; flatten; opt; lower --neg-edge keep`, then `abc --gates`,
`dfflibmap`. `lower` (`src/passes/lower/mem.c`) turns every `$mem` into one flip-flop per bit plus read and
write multiplexers, so a memory of N words by W bits costs N*W flip-flops (sky130_fd_sc_hd `dfxtp_1`, about
20 um^2 each) plus the read mux tree and the write-enable decode. A memory over `--max-mem-bits` (default 2^22 bits,
`ODIN3_LOWER_MAX_MEM_BITS`) is refused: `'$mem' ... has N bits (cap M); hardmap it or raise --max-mem-bits`. So
small memories become flop arrays (large, but placeable) and big ones stop the flow. On the FPGA path the same
`$mem` is kept: `hardmap` maps it to `single_port_ram`/`dual_port_ram` of the architecture library
(`lib/vtr.o3lib`), split by width and depth with the project `map`/`split` rules.

## 2. What the installed PDK offers (checked 2026-11-05)

`work/tools/pdk/sky130A` is open_pdks 792ef67037d7 and holds one library, `libs.ref/sky130_fd_sc_hd` (standard
cells only: cdl, gds, lef, lib, mag, maglef, spice, techlef, verilog). No SRAM macro (no `sky130_sram_*`
directory, no `*sram*` or `*openram*` file anywhere under the PDK) and no `sky130_fd_bd_sram` library. Macros of
that kind (OpenRAM-generated `sky130_sram_*` blocks, with LEF, GDS, Liberty, Verilog model, SPICE) would be an
extra library to install into the PDK with `ciel`; I have not verified here which libraries and macro sizes the
pinned version offers, so the sizes and names below are to be read from the installed files, not assumed.

## 3. Options

A. Memory as a macro through the mapping path (preferred, same shape as the FPGA path).
  - A `.o3lib` for the macro (like `lib/vtr.o3lib`'s `single_port_ram`): `cell <macro> hard`, with address,
    data, write-enable, clock and the macro's bit width and depth as fixed parameters; generated from the macro's
    Liberty and Verilog model by a small tool next to `tools/silicon/liberty_to_o3lib.py` (that tool only emits
    gates today; hard cells need the bus-pin and function-less form).
  - The silicon script gets `read_techlib` of both libraries and a `hardmap` step before `lower`, with a
    project `map`/`split` rule choosing the macro and the tiling (N macros wide, M deep, with the address-decode
    glue left as soft logic). `hardmap` and `split` already exist (PASSES.md `hardmap`, `.o3proj` `map`/`split`).
  - Provenance: a hard cell keeps the SOURCE of the `$mem` it replaced, as on the FPGA path (a `$mem` node maps to
    one hard-cell node per tile, each with the memory's chain), so the layout join still reaches the HDL.

B. Documented black box (cheap, user does the work).
  - The RTL instantiates the macro module by name; an Odin III `(* blackbox *)` module (or `--auto-blackbox`)
    is kept as one cell type and written by `write_verilog` as an instance. The flow passes the macro's LEF
    and Liberty to OpenROAD, its GDS to KLayout and Magic, its SPICE/netlist to Netgen for LVS.
  - No memory inference: the designer replaces the array by a wrapper by hand. Works for any macro, never
    changes the design silently.

C. Keep today's behaviour, add a WARNING per memory flattened to flip-flops in the silicon path with the bit
   count and the area estimate (N*W flip-flops). Zero risk; no layout benefit.

## 4. Changes the flow needs for A or B (all outside the C core except where noted)

1. `discover` in `silicon_flow.py`: find the macro library (`--macro-lib DIR`, or `libs.ref/<name>` of the PDK):
   LEF, Liberty, GDS, Verilog model, SPICE per macro. `missing()` names it when a design needs one.
2. `openroad_flow.tcl`: `read_lef` and `read_liberty` of each used macro (today only
   `sky130_fd_sc_hd.lef` and the tt Liberty at lines 25 to 27); macro placement before global placement
   (`macro_placement` or a fixed `place` from a floorplan file; macros are large, so the floorplan must be sized
   from them, not from standard-cell area); PDN straps over the macro (the macros have power rings, usually
   met4/met5); routing blockage obstructions come from the LEF.
3. `klayout_gds.py`: read the macro GDS next to `sky130_fd_sc_hd.gds`.
4. `magic_extract.tcl` and `netgen_lvs.py`: read the macro GDS or abstract and its SPICE. LVS of a macro needs
   its netlist as a black-box subcircuit, as for standard cells.
5. `census.py` / `kind_of`: classify macro instances as `macro`, not as an unknown master.
6. Core (option A only): a tiling `hardmap` rule for a memory over a fixed-shape macro (width and depth
   splits exist for the FPGA RAM; the silicon macros are fixed-aspect, so the split rule needs an extra "round up
   and report the waste" report line).
7. Equivalence: `equiv-check` cannot prove a macro. The `equiv` step must treat a macro as a black box on both
   sides and say so (WARNING, as the brief rule for equiv failures: continue). Macro behaviour is checked in
   simulation against the macro's Verilog model, not by the equivalence step.

## 5. Costs and risks

- Area: for 4 KiB (32768 bits) the flip-flop version is about 32768 x 20 um^2 = 0.66 mm^2 before the read
  muxes; a macro of that capacity would be a fraction of it. Numbers for the actual macros must be read from
  their LEFs; none is installed.
- Timing: macro clock-to-output delays are long and the macro is a hard IP with setup constraints on the
  address; the Liberty carries them but the flow's constraint script must not assume zero-delay memories.
- Tools: OpenROAD macro placement and PDN on the installed OpenROAD (2022 build, see the `route.guide` comment
  in `run_openroad`) are untested here; this is the biggest unknown.
- Behavioural difference: an OpenRAM macro has a synchronous read (data one cycle after the address) and its
  own write-mask and chip-select polarities. A `$mem` with an asynchronous read port cannot be mapped to it
  without a wrapper; `hardmap` must refuse (not silently add a register) when the read is asynchronous.
  darkriscv's register file reads asynchronously, so for that design option A would apply to the
  cache memories only and the register file stays flip-flops.

## 6. Decision and status

Implementation is **too big for now**: the PDK has no macros, so nothing can be built or tested end to end
(no LEF to place, no Liberty to time, no GDS to merge, no SPICE for LVS). Recommended order when a macro library
is installed: (1) option C now (a WARNING per flattened memory; a small `lower` change, no macro needed), (2)
option B on one design with a hand-written wrapper, to prove the OpenROAD, KLayout, Magic and Netgen changes of
section 4 items 1 to 5, (3) option A for synchronous-read memories using the existing `hardmap` and
`split` machinery.

Open items for the controller: which macro library to install into the pinned PDK (network access and a
decision on the licence/provenance of OpenRAM-generated macros), and whether a memory that is only a
register file (asynchronous read, small) should stay in flip-flops by rule.
