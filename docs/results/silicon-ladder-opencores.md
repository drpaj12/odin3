---
orphan: true
---

# Silicon ladder on 7 OpenCores designs (2026-11-04, rerun 2026-11-05)

Purpose: run the Odin III to OpenROAD to GDS flow (no Yosys, `tools/silicon/silicon_flow.py`, see
{doc}`../user/silicon-flow`) on real OpenCores designs that have testbenches, smallest first, and record how far
each one gets. Research question: does provenance from the GDS back to the HDL survive on designs larger than the
two micro designs of v1.4. The first run (2026-11-04) logged the failures; the rerun (2026-11-05, section "Rerun after the fixes") follows the fixes of the brief odin3-silicon-fixes-3 and is the current result. The first-run text below is kept for the record where it is marked "first run".

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

## Rerun after the fixes (2026-11-05)

Fixes merged on `main` (one PR each; PR, squash-merge commit):

| # | Fix | PR | Merge |
|---|---|---|---|
| 1 | `netlist` strips `signed` from declarations (OpenROAD STA-0164, wb_lcd) | #351 | 53fa8df0 |
| 2 | `gates_objects`: one `cone` link per chain span to the `hdl` object, not all-pairs gate/IR links (sha3_low: 278,589,437 links before, 131,469 after) | #353 | 5e663b93 |
| 3 | die edge kept 0.17 um from the last met1 to met4 track (DRT-0255 on des) | #357 | 5cc81897 |
| 4 | wb_lcd equivalence: no engine proves it; attempts recorded, WARNING stays | #355 (docs) | 57f4f999 |
| 5 | `tools/silicon/gate_sim.py`: gate simulation from an X, a 0 and a 1 start state (mmuart) | #352 | c0f83088 |
| 6 | `silicon_flow.py --param NAME=VALUE` (i2c) | #354 | 28b7e719 |
| 0 | ladder logs without home paths (`python_tools` was red on main) | #356 | f2586ab9 |

Found by the rerun and fixed (not in the brief):

| # | Fix | PR | Merge |
|---|---|---|---|
| 7 | constant assigned to a bit select with a space (`assign n [0] = 1'b0;`) now gets a tie cell (DRT-0305 `one_`/`zero_` on cordic and sha3_low); the not-proven reason is the `equiv-check:` line, not ABC's trailing `hi` | #358 | 2dbc9405 |
| 8 | external tool limit 6 h (Magic extraction of des took more than 3600 s) | #359 | ed751f18 |
| 9 | `probe_p_8`, `probec_p_8` dont-use (repair_design put a probe cell on a net of sha3_low; global route: `net net23 initial #node != #rpin`) | #360 | 89e8ce62 |
| 10 | name survival in one pass per file (the per-name search ran more than an hour on des) | #365 | e2ade2e4 |

Binary: `odin3 1.4.0` release built with `tools/build-locked` from a tree whose `src`, `include`, `lib`, `adapters` and `CMakeLists.txt`
are identical to `main` at 2dbc9405 (`commit 21bda392` in `--version`; the C sources last changed in a6c45390). Python flow and Tcl: `main` at the commit
of the fix list above that was current when each design started (wb_lcd and cordic: 2dbc9405; des: ed751f18; sha3_low: 89e8ce62 for the flow, e2ade2e4 for
extract onward; mmuart and i2c: 89e8ce62 or later); fixes 8 to 10 do not change any result of the designs that ran before them. Two heavy-run slots, so
wall time and peak memory below include a second job on the machine. Scripts: `silicon-ladder-opencores-logs-rerun/scripts/`.

### Results

| Design (LUTs) | Gate cells (Odin III) | Steps reached | Core area um2 | Util | WNS / TNS ns | DRC | LVS | Equiv | Recover (join) | Sim RTL / final.v | Wall s | Peak MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| mmuart (126) | 488 | 11 of 11 | 14589 | 26.9% | +6.44 / 0 | 0 | match uniquely | equivalent | 497 of 497 joined; 488 with HDL chain | pass / x-pessimism, 61 of 61 from a 0 or 1 start | 271 | 538 |
| simple_spi (189) | 687 | 11 of 11 (first run, unchanged) | 20365 | 27.3% | +7.12 / 0 | 0 | match uniquely | equivalent | 704 of 704; 687 | pass / pass | 244 | 549 |
| i2c (316) | 984 | 11 of 11 | 30669 | 27.3% | +6.40 / 0 | 0 | match uniquely | equivalent | 1001 of 1001; 984 | pass / x-pessimism, passes from a 0 or 1 start (testbench copy, see below) | 427 | 557 |
| wb_lcd (1049) | 3020 | 11 of 11 | 98995 | 28.3% | +4.90 / 0 | 0 | match uniquely | not proven (dsec UNDECIDED, WARNING) | 3141 of 3141; 3020 | pass / x-pessimism, passes from a 0 or 1 start | 1840 | 1459 |
| cordic patched (1153) | 7536 | 11 of 11 | 188397 | 26.2% | +5.56 / 0 | 0 | match uniquely | equivalent | 7663 of 7663; 7536 | pass / pass | 4469 | 1083 |
| des (1877) | 18686 | 11 of 11 | 516946 | 28.2% | +7.21 / 0 | 0 | match uniquely | equivalent | 19208 of 19208; 18686 | pass / pass | 19995 | 3136 |
| sha3_low (5035) | 17565 | 11 of 11 (extract, lvs, recover in a second run on the same directory) | 537507 | 36.5% | +4.35 / 0 | 0 | match uniquely | equivalent | 18159 of 18159; 17566 | pass / pass | 44694 (21850 to the 6 h limit inside extract, then 22844 for extract, lvs, recover) | 4624 |

The four designs that stopped in the first run now all run the whole flow. sha3_low passes `map`, the tie-cell step, place, route (DRC 0, WNS +4.35 ns, hold +0.42 ns), GDS and its `final.v` simulation (`Good!`); its flat Magic extraction is the slow step (see below): the first run reached the 6 h run-script limit inside `extract`, and `extract,lvs,recover` then ran again on the same directory (`run_tail.sh`, 12 h limit, 22844 s, peak 4624 MB): LVS `Circuits match uniquely`, join 18159 of 18159 cells (17566 with an HDL chain), 0 names in the GDS and in the extracted SPICE. Because that second run restarts the provenance steps, sha3_low's `prov/` index holds only extract, lvs and recover. No size limit was hit.
Names in the GDS and the extracted SPICE: 0 on every completed design. No cell was removed or renamed. The numbers are in `silicon-ladder-opencores.tsv`
(first-run rows for simple_spi; the other rows are the rerun). The first-run table is below.

### What each fix changed

1. wb_lcd: `place` now runs (`signed` stripped). Whole flow, LVS match, DRC 0.
2. `map` memory on sha3_low: peak RSS of the whole flow 508 MB (first run: killed at 6,141 MB in under a minute); `gates_objects` alone 137 MB and 4.3 s.
   cordic: 291 MB in the same step. The gate provenance links now point to the `hdl` span objects (kind `cone`); the IR cells of a span reach the same object by their `fine` links.
3. des, cause: the die side was 732.615 um, 0.065 um beyond the last met2 track and 0.085 um beyond the last met1 track (left edge: 0.23 and 0.17). The router puts the
   access point of a pin on the die edge at the last track and the via enclosure leaves the die. The four failing route boxes were the right-edge column that holds the
   output pins; the net name in the message (`o3__n730`) is not on that edge (its guides are 300 um away). The die is now enlarged to the next edge with at least 0.17 um
   (733.185 um; the core and the placement are unchanged). des routes with 0 DRC violations.
4. wb_lcd equivalence: `dsec` with default settings, `-r`, `-F 1`, `-F 8`, `-m`, `-rm`, `-a`, `miter; strash; dprove`, `miter; strash; scorr; dretime; scorr; dprove`
   and `miter; strash; scorr; pdr` (900 s) all end UNDECIDED or time out; the RTL BLIF has 660 registers, the gate BLIF 636, and no register name survives. The WARNING stays
   (`equiv: not proven: equiv-check: unproven: ABC dsec ended UNDECIDED ...`).
5. mmuart `final.v`: X pessimism, not a functional difference. `tx_bitcount[0]` has the next-state logic `nor(a21oi(.., bitcount0), nor(.., ~bitcount0))`: for a
   write strobe of 1 it is 0 whatever the old value is (equivalence is proven), but a gate simulator computes X from an X old value and keeps it. The registers have no reset
   in the RTL (they are loaded by `tx_wr`). With every flip-flop started at 0 or at 1, all 61 checks pass. `gate_sim.py` runs the three starts and reports `x-pessimism`
   with a WARNING; it applies to mmuart, i2c and wb_lcd.
6. i2c: the netlist is built with `--param ARST_LVL=1'b0` (the testbench's value) and the testbench copy loses its `#(.ARST_LVL(0))`. The gate simulation then runs. From an X start
   it shows `Expected NACK, received ACK`; the cause is not the netlist but the testbench: its slave model has a 4-word memory and the testbench reads word 4, so the slave drives X on SDA; the RTL
   takes X as false in an `if`, the gates propagate it. With the slave memory declared as 16 words initialised to 0 (a copy of the testbench, `tb/i2c/i2c_slave_model.v` in the rerun logs) the RTL and the
   gate netlist (from a 0 or a 1 start) both pass; the RTL passes with the original testbench too.

Not fixed: the single-file input (the designs are still concatenated by hand).

### Rerun problems that were not flow bugs

- The machine had two heavy-run slots and another lane held one for hours; queued jobs waited (wall times above start when the job started).
- The first rerun of des stopped at `extract` (the 3600 s tool limit, fix 8); the first sha3_low rerun stopped at `place` (the probe cell, fix 9); the second ran `place` to `gds`, and
  hit the 6 h flow limit of the run script inside the flat Magic extraction (about 3.5 h for a 74,336-instance layout, the `.ext` file is 0.8 GB). Its extract, lvs and recover steps were then run
  by themselves on the same directory (`run_tail.sh`), so its `prov/` index holds only those steps.

## First run (2026-11-04)

### First-run results

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

### First run: where the ladder stopped, and why (exact messages)

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

### First run: functional check (final.v)

- **mmuart: FAIL.** The RTL passes the 61 checks; `final.v` plus sky130 models fails 42 of 61 (`tx byte 0 got xX want 55`, no `tx_irq`, `rx byte 0 got 000000xx`), with
  unknown values on the transmitter and receiver paths; the CSR divisor checks pass. Equivalence was proven (`abc dsec`) and the flow ran to LVS. Not diagnosed: it may be X pessimism
  (the RTL has no reset on several registers and no `initial`), or a real difference; the run does not separate the two.
- **simple_spi: pass** (same `Testbench done` and no compare error as the RTL).
- **i2c: not run.** `tst_bench_top.v:140: error: parameter 'ARST_LVL' not found in 'tst_bench_top.i2c_top'`: the testbench instantiates with `#(.ARST_LVL(0))`
  and the flow netlist has no parameters (it was built with the default `ARST_LVL=1`). The RTL passes.
- wb_lcd, cordic, des, sha3_low: no `final.v` (the flow stopped earlier). The RTL passes for all seven.

### Provenance (first run)

On the three designs that completed, every placed logic cell joined back by cell type and location (join rate 1.0); the share with an HDL chain was 488 of 497, 687 of 704 and 979 of 996: the
rest are the clock-tree buffers (physical origin). The Odin III names were kept in the DEF through OpenROAD (no removals or renames). No new result on provenance exists for the four larger designs.

### First run: routed to the controller

1. `netlist` step does not strip `signed` from wire declarations (OpenROAD 2022 parser rejects it): wb_lcd.
2. `place`: OpenROAD errors are reported as "out of scope" (exit 3), which hides real flow failures (STA-0164, DRT-0255).
3. `map`: memory of `gates_objects` (silicon_flow.py:583): cordic and sha3_low die at 6 GiB.
4. `detailed_route` DRT-0255 on des at 25% utilisation.
5. Equiv: `abc dsec` UNDECIDED on wb_lcd (peak 4.6 GB for the run).
6. Gate-level sim of mmuart gives x; i2c testbench parameter override cannot apply to the netlist.
7. The flow takes one design file: multi-file designs have to be concatenated by hand.
