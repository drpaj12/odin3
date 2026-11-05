---
orphan: true
---

# Silicon ladder on 7 OpenCores designs (2026-11-04)

Purpose: run the Odin III to OpenROAD to GDS flow (no Yosys, `tools/silicon/silicon_flow.py`, see
{doc}`../user/silicon-flow`) on real OpenCores designs that have testbenches, smallest first, and record how far
each one gets. Research question: does provenance from the GDS back to the HDL survive on designs larger than the
two micro designs of v1.4. This lane logs failures; it does not fix Odin III or the flow.

The numbers are in `silicon-ladder-opencores.tsv` (for Mimir). Raw step output, measure logs, sim logs and the scripts are in
`silicon-ladder-opencores-logs/`.

## Setup

- Odin III binary: `build/rel-main2/odin3`, release, commit `4c91edff1303e7c435f1f5e0bae16f0dc037bc52` (built 2026-11-02).
  The Python flow (`tools/silicon/`, `lib/sky130hd.o3lib`) is `main` at `30c1061d` (includes #333 and #336). A release build of
  `30c1061d` was queued behind two heavy-run jobs for 40 minutes and then dropped; the brief allows `rel-main2`. The binary is
  older than the Python by the sim_tb, tribuf-lower and project-dump changes (#320 to #329), none on the silicon path of these designs, but this is not proven.
- Tools: OpenROAD `2.0_3175_gf12e2f474`, Magic 8.3.465, Netgen 1.5.272, KLayout 0.30.12, SKY130A `sky130_fd_sc_hd` (as in the flow manual), under `work/tools`.
- Each design runs under `tools/heavy-run` (one slot: 6 GiB memory cap, no swap). Clock: 10 ns, set by the flow on the named clock port
  (the flow fixes the period). Memory is the peak RSS of the whole flow (`/usr/bin/time`).
- Inputs: the flow takes one file, so each design is the source files concatenated in the order of the opencores-runs config into
  `design.v`, with include files beside it. `verilog_cordic_core` is the Mimir-patched variant (`opencores-synth/verilog_cordic_core/cordic.v.patch`,
  identical to `work/opencores-runs/cordic_synth/cordic.v`). Testbenches: Mimir's for mmuart, wb_lcd and cordic
  (Mimir `d482c2f5`); the project's own for simple_spi (with Mimir's two fixes), i2c, des and sha3_low.
- Measures: cells, area and utilisation, WNS/TNS (setup, worst corner tt_025C_1v80, placement-estimated parasitics, 10 ns clock, hold worst slack also given)
  come from `measure.tcl` run on `final.odb`. Utilisation here is logic area (no tap, fill) over core area. DRC is OpenROAD's route DRC report
  (violation count); the flow has no other DRC deck. LVS is Netgen's final line. Recover share is the name-free join result.
- Functional check: iverilog on the design's testbench, once on the RTL and once on `final.v` with the sky130 Verilog models
  (`-DFUNCTIONAL`).

## Results

| Design (LUTs) | Gate cells (Odin III) | Steps reached | Core area um2 | Util | WNS / TNS ns | DRC | LVS | Equiv | Recover (join) | Sim RTL / final.v | Wall s | Peak MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| mmuart (126) | 488 | 11 of 11 | 14589 | 27.0% | +6.42 / 0 | 0 | match uniquely | equivalent | 497 of 497 joined; 488 of 497 with HDL chain | pass / FAIL (42 of 61 checks, x values) | 163 | 538 |
| simple_spi (189) | 687 | 11 of 11 | 20365 | 27.3% | +7.12 / 0 | 0 | match uniquely | equivalent | 704 of 704 joined; 687 of 704 with HDL chain | pass / pass | 244 | 549 |
| i2c (316) | 979 | 11 of 11 | 30669 | 27.2% | +6.37 / 0 | 0 | match uniquely | equivalent | 996 of 996 joined; 979 of 996 with HDL chain | pass / not run | 440 | 562 |
| wb_lcd (1049) | 3020 | 5 of 11, stop at `place` | n/a | n/a | n/a | n/a | n/a | not proven | n/a | pass / not run | 206 | 4598 |
| cordic patched (1153) | 7534 | 2 of 11, stop in `map` | n/a | n/a | n/a | n/a | n/a | not run | n/a | pass / not run | 27 | 6141 |
| des (1877) | 18686 | 5 of 11, stop at `place` | n/a | n/a | n/a | n/a | n/a | equivalent | n/a | pass / not run | 675 | 4216 |
| sha3_low (5035) | 17565 | 2 of 11, stop in `map` | n/a | n/a | n/a | n/a | n/a | not run | n/a | pass / not run | 48 | 6141 |

"Steps reached" counts steps that printed `ok`. For the two designs killed in `map`, `read` and `elab` printed no line (the flow prints all lines at the end
and was killed first); the two step files `01-read.json` and `02-elab.json` exist, so 2 steps finished. The three clean runs: cells inserted by the flow were tap cells
(184, 270, 396), clock buffers (9, 17, 17) and fill (1475, 2134, 3189); no cell was removed or renamed; `repair_design` resized 56, 136 and 144. Names in the GDS and in
the extracted SPICE: 0 (as in v1.4). WNS is large and positive because the 10 ns clock is easy for these cores and the flow runs no timing optimisation target.

## Where the ladder stopped, and why (exact messages)

1. **wb_lcd, step `place`.** Exit 3, recorded by the flow as out of scope: `[ERROR STA-0164] <path>/clean.v line 36, syntax error` (OpenROAD
   `openroad_flow.tcl:28`, `read_verilog clean.v`). Line 36 of `clean.v` is `wire signed [31:0] o3_lcd_pos ;`: the 2022 OpenROAD Verilog
   reader does not accept `signed`, and the `netlist` step does not strip it. This is a flow limit classified as "out of scope" although the design is not out of scope.
   Also: `WARNING equiv: not proven: The unsolved reduced miter is written into file "sm01.aig".` (`equiv-check: unproven: ABC dsec ended UNDECIDED`); the peak memory of the whole run
   was 4.6 GB (the step is not isolated). No further steps ran.
2. **des, step `place` (the OpenROAD run stops in detailed routing).** Exit 3, out of scope:
   `[ERROR DRT-0255] Maze Route cannot find path of net o3__n730 in worker of routeBox ( 724495 193195 ) ( 732615 241495 ).` (four such lines, then
   `Error: openroad_flow.tcl, 139 DRT-0255`; line 139 is `detailed_route`). Placement, clock tree, repair and global route (`s00` to `s09`) completed in 15 s of OpenROAD time; the
   placed design has 26,125 instances before routing. Equiv: equivalent. Again a flow failure classified as out of scope. Total 675 s, 4.2 GB.
3. **verilog_cordic_core (patched), step `map`.** `Command terminated by signal 9` at 26.8 s, peak RSS 6.14 GB (the 6 GiB slice cap): the kernel killed the flow, so no
   step line was printed. Re-run with only `read,elab,map,equiv,netlist` and again without `equiv`: the same kill at 26 s, so it is not the equiv step. Odin III's `map` itself
   finishes (`gates.blif`, `g.v`, `prov.csv` with 7,534 rows are on disk).
4. **sha3_low, step `map`.** The same signal 9 at 48 s, 6.14 GB, with `prov.csv` of 17,565 rows on disk.
   For 3 and 4, a sampled run (`sha3_low_diag`, a Python stack dump at 20 s and `ps` every 2 s) puts the memory in the Python process, not in Odin III or ABC: the stack is
   `silicon_flow.py` `_do_step` (line 741) in `gates_objects` (line 583), RSS 4.2 GB at 2 s intervals rising to 5.4 GB before the kill. The function links each gate to
   every IR cell that shares any source span of its chain, so the link lists are large for designs with long `always` spans. Cause not analysed further. The diagnostic stack is
   `silicon-ladder-opencores-logs/sha3_low/diag_stack.log`.

Nothing else hit a time limit; no step ran past the 6 h flow limit. The 9 GB `--xl` slot was not tried (it is for the controller only).

## Functional check (final.v)

- **mmuart: FAIL.** The RTL passes the 61 checks; `final.v` plus sky130 models fails 42 of 61 (`tx byte 0 got xX want 55`, no `tx_irq`, `rx byte 0 got 000000xx`), with
  unknown values on the transmitter and receiver paths; the CSR divisor checks pass. Equivalence was proven (`abc dsec`) and the flow ran to LVS. Not diagnosed: it may be X pessimism
  (the RTL has no reset on several registers and no `initial`), or a real difference; the run does not separate the two.
- **simple_spi: pass** (same `Testbench done` and no compare error as the RTL).
- **i2c: not run.** `tst_bench_top.v:140: error: parameter 'ARST_LVL' not found in 'tst_bench_top.i2c_top'`: the testbench instantiates with `#(.ARST_LVL(0))`
  and the flow netlist has no parameters (it was built with the default `ARST_LVL=1`). The RTL passes.
- wb_lcd, cordic, des, sha3_low: no `final.v` (the flow stopped earlier). The RTL passes for all seven.

## Provenance

On the three designs that completed, every placed logic cell joined back by cell type and location (join rate 1.0); the share with an HDL chain was 488 of 497, 687 of 704 and 979 of 996: the
rest are the clock-tree buffers (physical origin). The Odin III names were kept in the DEF through OpenROAD (no removals or renames). No new result on provenance exists for the four larger designs.

## Routed to the controller (not fixed here)

1. `netlist` step does not strip `signed` from wire declarations (OpenROAD 2022 parser rejects it): wb_lcd.
2. `place`: OpenROAD errors are reported as "out of scope" (exit 3), which hides real flow failures (STA-0164, DRT-0255).
3. `map`: memory of `gates_objects` (silicon_flow.py:583): cordic and sha3_low die at 6 GiB.
4. `detailed_route` DRT-0255 on des at 25% utilisation.
5. Equiv: `abc dsec` UNDECIDED on wb_lcd (peak 4.6 GB for the run).
6. Gate-level sim of mmuart gives x; i2c testbench parameter override cannot apply to the netlist.
7. The flow takes one design file: multi-file designs have to be concatenated by hand.
