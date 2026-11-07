# Handover: Odin III simulator parity lane (odin3-sim), written 2026-11-06

Brief: /home/jamiespa/odin3-ws/work/controller-scripts/briefs/odin3-sim-parity.md (Rules section applies).
Worktree: /home/jamiespa/odin3-ws/work/odin3-sim (build/debug only; keep one preset; rm build/ when the lane's last PR merges).
Builds only via /home/jamiespa/odin3-ws/odin3/tools/build-locked --preset debug. Merge via work/controller-scripts/mw.sh drpaj12/odin3 <pr> <full sha>. Never `cd X && write`; no sleep loops.
Design: docs/specs/2026-11-05-sim-4state-design.md (+ addendum 1, section 10). Testbench spec: docs/specs/2026-10-31-testbench-design.md (7c).

## Merged so far
- #377 design note (a9228917). #379 gap 10 FIFO wake order + `tb race:` WARNING (8631abde). #380 gap 9 comment at include EOF (8495dd74; #340 closed).
- #381 SystemVerilog macro operators `` `` ``, `` `" ``, `` `\`" ``, `__FILE__`, `__LINE__` in the preprocessor (4cd4349c). sdram_16bit's simulation.svh line 19 preprocesses; the design was not re-run (its upstream tb is SystemVerilog, other constructs may refuse).
- #382 phase 2a (0b0ee7fb): src/sim/word4.{h,c} (4-state words, planes v/u, code 0,1,x=2,z=3 = odin3_bit), src/sim/sim4.{h,c} (4-state cell models: gates, consts, $_DFF_*/$_DLATCH_*/$_FF_, the 68 dffx types, $lut, $mux, $pmux, $eq, $ne, $reduce_or, $reduce_bool, $logic_not; generic pessimistic wrapper for other comb types; stateful types without a model are refused), `odin3_sim_options.four_state`, odin3_sim_{set_input4,get_input4,get_output4,is_four_state}, `proc` sets node attribute ODIN3_ATTR_RTL_SEL ("odin3 rtl_sel", hidden from RTLIL/JSON/Verilog writers) on the mux/pmux/eq/reduce_or/logic_not cells it builds from if/case, `lower --keep-sel` leaves those cells as word cells.
- #384 phase 2b (dc8b4f4d): 4-state, up to 1024-bit testbench values (src/sim/tb_val.c, tb_ops.c, tb_expr.c ...) behind `sim_tb --4state`. DEFAULT STAYS 2-state (controller ruling: wb_lcd regressed in 4-state after opt). `--2state` accepted as a no-op. x micros: tests/tb/micro/x*_tb.v (run with --4state by tests/tb/run_micros.sh and run_vcd.sh; `<n>.flags` holds extra `lower` args such as --keep-sel).

## Next step 1 (controller's order): wb_lcd fails in 4-state after `opt`
Facts: with `--4state`, wb_lcd after `read_verilog; proc; opt; lower --keep-sel` prints `FAIL status not busy during init` (status reads `...0x`); with `proc; lower --keep-sel` (no opt) it equals iverilog; 2-state mode passes. Find the real cause by BISECTING which opt sub-pass changes the x behaviour on an `if` with an x condition (run the opt passes one at a time: `opt_expr`, `opt_merge`, `opt_muxtree`, `opt_clean`, dff/enable folding ... list them with the pass manager; compare 4-state outputs after each). Do not guess. Hypotheses only (unverified): an `if` folded into a flip-flop enable (`$dffe`, `$sdff*`: 4-state model of enable/reset with x control merges instead of RTL else-side), or the rtl_sel attribute dropped when a pass rebuilds/merges cells (opt_merge drops the mark when it merges a marked and an unmarked cell). Fix it (mark preserved per pass with a unit test per pass, or 4-state flop models taking the RTL meaning for x enable/reset). The default may flip to 4-state only when every design that passes in 2-state also passes in 4-state (mmuart, verilog_cordic_core, wb_lcd, then the rest).
Run times: the debug build runs wb_lcd for ~17-21 min; use a release build via tools/heavy-run for iteration if needed (build to a separate dir only if the controller allows another build tree).

## Gaps list
- DUT-side tri-state: `lower` turns `assign y = oe ? d : 'bz` into a plain driver (warned); before `lower` sim_tb refuses `$tribuf` (no model). Needs a $tribuf hook plus resolution cells for multi-driver nets in odin3_sim (design note section 6).
- `repeat (x)`: zero iterations (IEEE); iverilog 12 loops forever; no micro.
- Shifts/div/mod/pow have no word-level sim hooks, so after `lower` they are gate-level (x amount gives merge-like results, not IEEE all-x).
- opt_merge can merge an if-mux with an identical ?:-mux; the merged cell loses the mark.
- Not yet done: phase 3 `real` (vga_lcd, sdr_ctrl, uart2spi; clock must be counted in timescale-precision ticks so `#(ClkPeriod/2)` with 10.0 works), phase 4 speed (dblclockfft 1200 s, reed_solomon_decoder 900 s: profile with perf first), phase 5 parity sweep script (every Mimir OpenCores tb under iverilog and Odin III: pass / identical / first differing line; results to the controller for the Mimir sim_odin column). There is no single driver for the Mimir design set yet: per-design commands are in /home/jamiespa/odin3-ws/mimir/data/benchmarks/opencores-tb/<name>/README.md and runs under /home/jamiespa/odin3-ws/work/opencores-tb-runs/.
- Designs refused by constructs outside the 4-state work: cordic (`for-loop condition is not constant`, Yosys rejects that too), simple_spi (parameter override on a DUT instance; needs tb-side behavioural modules spi_slave_model/wb_master_model, gap 3), usbhostslave (`defparam` in harness, three behavioural modules, ambiguous_top), uart2spi (real), sdram_16bit (SystemVerilog tb), pipelined_fft_256 (parse error was gap 9, fixed in #380; re-run), sha3_* (>64-bit now supported by --4state; re-run), i2c/spi/des (x/z now supported; re-run with --4state and check other refusals), gap 5 non-constant for-loop bound = not legal for Parmys/Yosys either (no fix).
- test_abc_adapter fails locally in the debug (ASan/LSan) build with a LeakSanitizer report inside third_party/abc; not checked on main.
- Flaky under `ctest -j3` load (pass alone): lower_cells, opt_rules*, opt_simcheck_rules, sim_tb_hier_flow, sim_vcd_check, test_tmp_cleanup.

## Numbers
- Mimir sim_odin passing today: 3 (mmuart, verilog_cordic_core, wb_lcd). mmuart re-verified after #381 (PASS 61 checks). Not re-run: the others.
- Perf: mmuart debug build 16.5 s baseline, 19.4 s with --4state (1.18x), --2state default same as before.
- Lint gate: tools/lint.sh in background, wait for the `lint: ` line (~10 min). Use fresh small agents for lint fix rounds.

## 2026-11-06 controller note: #385 merged
- PR #385 (fix/opt-4state-xcond) squash-merged 7e95536f: rtl_sel kept through opt on swapped muxes and folded FFs (lower connect/kit, opt dff_merge/dff_muxes/expr_logic, sim4), micro tests/tb/micro/xff + unit tests. CI green first time.
- wb_lcd --4state after opt: fixed by #385 per the sim agent. Default NOT flipped. Next sim agent: re-run mmuart, verilog_cordic_core, wb_lcd in both modes on a fresh binary, then the default-flip check.
- The sim agent did not write its own final section (agent ended); worktree parked detached at origin/main.

## Fixes lane (2026-11-07)
- Item 12 (run-micro): premise stale. .claude/skills/run-micro/run-micro.sh already runs CTest micro sweeps (no golden/<arch>/micro lookup). Ran on a fresh debug build: 15 passed, 0 failed, 0 skipped. No PR.
- Item 13: #386 merged b1a35e00 (-g1 for ABC and slang in Debug; Odin III code and sanitizers unchanged). Debug build dir 20G before, 17G after (test_abc_adapter 169 MB to 58 MB). Each of ~300 test binaries still ~110 MB from Odin III's own -g. work/odin3-smalldebug removed.
- 14a: #387 eb57a6ce. Root cause: fake equiv-check matched "unp"/"ierr" in the whole path, including the random temp dir name; now only below --keep.
- 14b: #388 de741b5f. No LSan failure on main (adapters/abc/lsan.supp already covers it). Real local failure: ccache base_dir makes __FILE__ "../../adapters/abc/guard.c", test_adapter_assert_clean now accepts it.
- 14c: #389 71714a21. Root cause: TMPDIR /tmp/odin3-ctest-<hash of build dir> was rm -rf'd by test_tmp_cleanup while a second ctest on the same tree was running. Cleanup now removes entries older than 30 min. Not reproducible with one ctest -j3 (full 584 tests passed; 5x repeat of the subset passed).
- Worktree detached at origin/main 71714a21, clean; build/debug kept (built at the 14b/14c head, later main commits not rebuilt).

## 2026-11-07 vcd-all lane (odin3-vcd-all-sim brief)
- #391 merged d60a4bb5: `sim` / `sim_tb --vcd F --vcd-all [--vcd-names short|provenance|hash]`: one 1-bit variable per connected net, named as `write_blif --vtr` names it (naming run odin3_blif_vtr_name_top, new `module` request field + odin3_blif_names_net; src/passes/sim_netnames.c). sim_tb nests the DUT scope tree under the instance scope. Test `sim_vcd_all` (tests/tools/test_sim_vcd_all.py, golden tests/sim/vcd_all_counter.golden.vcd). Spec SW-5.
- rel-main5 = d60a4bb5 (1.4.0, ABI 7), /home/jamiespa/odin3-ws/odin3/build/rel-main5 (Release, all targets). rel-main3/4 untouched.
- Sim lane rerun on rel-main5 (scripts and logs: /home/jamiespa/odin3-ws/work/odin3-sim-runs/r5, results.txt). RTL view `proc; opt; lower` (4state: `lower --keep-sel`, `sim_tb --4state`). cordic uses the ladder design.v (work/power-review-data/sifix3-out/ladder2/verilog_cordic_core/design.v: line 178 loop bound made constant); raw cordic.v is still refused (for-loop condition not constant).
  mmuart 2s 1.8 s PASS 61 / 4s 1.3 s PASS 61; cordic 2s 1.7 s PASS 200 / 4s 1.6 s PASS 200; wb_lcd 2s 67 s PASS 307 / 4s 55 s PASS 307. Output text of 2s, 4s and iverilog identical on all three (INFO lines excluded).
- Default flip: every design passing in 2-state passes in 4-state; recommendation is to flip. Not flipped (needs controller go). Not run: the other Mimir designs in 4-state.
- Worktree work/odin3-sim parked detached at origin/main d60a4bb5; build/debug kept (stale). work/odin3-vcd removed (18G build freed).
- Bifrost P1 check (controller item 1): mmuart netlist-view Odin VCD (sim_tb --4state --vcd-all --vcd-names hash, rel-main5) against the iverilog gate activity.csv, P1 over known time (x excluded, as Bifrost): 284 matched nets, P1 diff max 1.24e-4, p90 8.1e-5, median 5.1e-7 (2-state Odin run: max 0.256, the x time). Activity diff max 2.2e-5 either way. The 0.256 was x time, not an Odin error.
- Default-flip check (rel-main5, release, via heavy-run; scripts/logs work/odin3-sim-runs/r5 and r6). Designs passing sim_tb 2-state today (other Mimir opencores-tb designs are refused by sim_tb per their READMEs): 2s -> 4s wall, checks, output text minus INFO identical:
  mmuart 1.8 s -> 1.3 s (61); cordic (ladder design.v) 1.7 s -> 1.6 s (200); wb_lcd 67 s -> 55 s (307); gng 16.0 s -> 3.6 s (11733); dblclockfft 1180 s -> 1100 s (12295; the 2-state run overlapped a full debug ctest, so timing noisy; ratio 0.93). No regression, worst ratio 1.0 -> flip.
- Flip PR: sim_tb default 4-state, `--2state` keeps the old interpreter, `--4state` accepted no-op. Project runner (run_testbenches) stays 2-state. Tests: sim_tb_default (new), tb_micros/tb_micros_2state, sim_tb_hier_flow and sim_tb_plusargs now pass --2state, sim_vcd_all golden regenerated (4-state, x until reset). Full ctest 587/587.
