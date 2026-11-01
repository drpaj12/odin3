# proc/opt time on an initial loop into register memories; derived clocks in the simulator (2026-11-01)

Branch `fix/initloop-clocks`. Found by the microcontroller lane (`2026-10-31-mcu-testbench.md`, "found, not
fixed"). Purpose: front-end and elaboration usability on real processors (Seeker-Proc bases).

## 1. The quadratic step

Reproduce: `dual_port_BRAM.v` of Arduino-on-RISC-V (32 bits, `ADDR_WIDTH` 6..9 = 64..512 words) with
`for (zero_k = 0; zero_k < RAM_DEPTH; zero_k = zero_k + 1) ram[zero_k] = 0;` in the module. The loop
makes the array a set of registers (mem2reg), the reads and the two blocking write ports become
switches over all the words.

Tool: `valgrind --tool=callgrind` on the Release build (perf has no call graph without frame
pointers; `perf record` gave addresses only). Release, one module, instructions of `read_verilog; proc`:

| words | read_verilog | proc | opt |
|---|---|---|---|
| 64 | 0.17 s | 0.16 s | 0.29 s |
| 128 | 0.73 s | 0.72 s | 0.84 s |
| 256 | 2.9 s | 2.6 s | 3.0 s |
| 512 | 11.8 s | 10.4 s | 11.8 s |

Each doubling is 4x in all three passes. Cells stay linear in the words (452 -> 836 muxes), nets too
(25,000 at 128 words), so the work is in the number of pairs (case, signal).

Steps found, in order of cost:

1. `proc_mux` (`mx_select` -> `mx_compare`) makes the `$eq` select of a case again for every signal
   (snippet) of the switch, as Yosys does, and Yosys's `opt_merge` joins the copies. A 128-word
   memory has 4096 one-bit-chunk signals and 128 cases per switch: 82,000 `$eq` cells (12,000 at 64
   words), all but 127 merged by `opt`. They are 12% of the instructions of `proc` and cost `opt_expr`
   (`round_once`, `odin3_expr_rule_eqneq`: 81,602 calls) and `opt_merge` the rest.
2. The elaboration (`pg_regs_apply` -> `regs_switch` -> `pg_sw_case`): each of the 128 cases starts
   `t1 = rv(bits)` for every target bit (Yosys's case temporaries), 128 x 4096 actions per switch, 2.2M
   input pins on the `$proc` node (`pg_emit`). This is the Yosys form and is the same size in Yosys;
   it is not changed here.
3. The `$pmux` of each word keeps an entry per case (value differs from the default x), which `opt`
   removes: 2M pins for `odin3_kit_topo` per `opt_expr` round.

Fix (step 1): `proc --share-selects`: one select cell per case of a switch, shared by all the
signals of the process (`mx_select` memoises the select bit by case index). It is an option and not the
default because `proc`'s cells are Yosys's one for one in `proc_rows` (goldens), `proc_diff_rows` and
`test_proc_mux`; the option removes only duplicate cells, and `opt` output is equal.

Result, `opt` output equal before/after (BLIF text equal after renaming `$n`/`$c` numbers by first
appearance) on the 64- and 128-word micro, after `opt` and after `lower`; on the MCU at 128 words per lane
(`tests/mcu`, bits 7, through `write_json` after `proc`, `opt` and `lower`: equal sizes, same cell mix).

Times, one module, Release (`proc` -> `proc --share-selects`): 64 words 0.153 -> 0.076 s, 128 words
0.55 -> 0.31 s (`opt` 1.1 -> 0.40 s), 256 words 2.6 -> 1.1 s, 512 words 10.4 -> 4.5 s.
MCU at 128 words per lane (bits = 9), Debug (ASan): read_verilog 2.9 s; proc 17.6 -> 5.8 s; opt
38.2 -> 19.7 s. The work is still quadratic through steps 2 and 3 (the Yosys form of a memory held
as registers with a variable-index write); removing it needs a different `$proc` form for such
writes and changes the `proc` output, so it is left for a design decision.

## 2. Derived clocks in the simulator

Oracles: Yosys 0.55 (`proc` of `tests/tb/micro/dclk.v`) keeps a divided, gated and muxed clock as the
`CLK` net of a `$dff`; VTR's flow treats a clock net by what it drives, whatever drives the net. So a
derived clock is a clock, and the simulator takes it (decision, agent default).

Model (`src/sim/build_clock.c`, `src/sim/eval.c`): an edge-triggered cell whose clock pin is on a net
that is not a primary input (not through identity buffers either) must have a driver; the build
records it as derived. If the net hangs on exactly one primary input that clocks nothing else, that
input is flagged as the clock (a gated clock whose only register is the gated one); with several, the
error names the cell and the net ("derived from N primary inputs and none of them clocks another
register") and the workaround; an undriven net keeps the old error. A combinational loop through a
clock net is the existing loop error (registers cut it).

Order on an input edge (`derived_edge`), as the active region and the non-blocking updates of a Verilog
simulator: the inputs rise; the logic settles so a clock made of the input shows its new value; the
cells on the input clock and the derived clocks that changed copy their inputs together; the outputs
settle; a derived clock that this changed (a register's output: a divided clock) triggers its cells in
the next round, with the values the first round left; rounds repeat until no derived clock changes (at
most 256, then an error: a register that clocks itself). `odin3_sim_settle_events` does the same rounds
after inputs change without an edge (a clock mux whose select changed makes an edge, as in iverilog);
`sim_tb` uses it. A design without derived clocks runs the old code unchanged.

Gate: `tests/tb/micro/dclk.v` with `dclk_tb.v` (divide by 2, divide of the divider, gated
`clk & en_q`, mux of the clock and the divider with a select that changes at run time): `run_micros.sh`
compares `sim_tb` with iverilog over 80 cycles, line for line: pass.

MCU timer unpatched (`8bit_TC.v`, patch 5 of `tests/mcu/prepare.sh` removed), `blink_interrupt`, 500
cycles, ODIN3_MCU_UPSTREAM set, Debug build: 64 KB RAM (`tb_mcu`): rtl 102 s, netlist 122 s, 500 lines
equal to iverilog; 128 words per lane with the zero-fill loop in the Odin III copy and
`proc --share-selects` (`tb_mcu_zero_fill`): rtl 71 s, netlist 43 s, 500 lines equal. So the timer
runs unpatched and the zero-fill loop is now in the tested design.
