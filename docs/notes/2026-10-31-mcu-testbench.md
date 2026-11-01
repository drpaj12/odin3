# Plan: Arduino-on-RISC-V microcontroller under Odin III testbench simulation (2026-10-31)

Purpose: first Seeker-Proc v2.0 base for Leman Russ III. Measure: Odin III runs a real
processor program, on the RTL and on the netlist, matching iverilog cycle for cycle on the ports
(PC, PORTB_data_out, PORTD_data_out).

Design: github.com/qiany21/Arduino-on-RISC-V (MIT), `quartus_project_revert_timer`, top
`single_cycle_BRAM_top`. Upstream stays outside this repo (read-only clone under `external/`); the
test skips when it is absent and a small patch set is applied to a scratch copy at run time. Only
the patches and the port-only testbench are committed, so the upstream licence text is not
needed in the tree (nothing of the upstream is vendored).

Steps
1. Front end: `$readmemh/$readmemb` first argument may be any constant string expression
   (string parameter, localparam, concatenation of string constants). Resolved at
   elaboration with the constant evaluator. As Yosys (`AstNode::readmem`, simplify.cc): the
   string is the constant's bytes with NULs dropped (so `{"0"+i, BASE}` works). Relative
   paths: Yosys tries the working directory, then the source file's directory. Odin III
   does the same under `--vtr-compat`; otherwise it reads the source file's directory (spec
   17 #6) and warns once per call when the working directory holds a file of that name too.
2. Regression micro + golden for the string parameter case.
3. Patch set for the design (PLL stub, `byte` identifier, `$time()`), port-only testbench,
   iverilog reference for 2-3 programs.
4. sim_tb on the RTL and on the netlist (proc; opt; lower; k6_frac mem32K with --vtr-compat
   and unmap if the RAM maps hard). Report cycles, wall time, memory, DFF count, first mismatch.
5. Fix only Odin III bugs found on the way; log Quartus-specific items as patches.
6. Lint, full debug ctest, CI, PR.

CTest `tb_mcu` (blink_interrupt, 500 cycles: both interrupt pulses) runs when the CMake cache
variable `ODIN3_MCU_UPSTREAM` (default: the environment variable of that name) names an
Arduino-on-RISC-V checkout; otherwise it skips (exit 77).

## Result (debug build, ASan + UBSan, one core, no other load)

Design: `single_cycle_BRAM_top`, 64 KB RAM as soft logic. Lowered: 132,446 `$_DFF_P_`, about
770,000 nodes after flatten (100,711 AND, 533,654 MUX). Port-only testbench `tests/mcu/mcu_tb.v`
(PC, PORTB, PORTD at every falling edge; the two interrupt pins pulse at cycles 200 and 400).
iverilog reference 0.02 to 0.2 s, 9 MB. Odin III `sim_tb`, identical to iverilog on every line
(iverilog's x at cycle 0 matches any value):

| program | cycles | RTL (proc; opt; lower) | netlist (write_blif, read_blif) |
|---|---|---|---|
| blink_interrupt | 6000 | 430.6 s, 834 MB | 392.7 s, 1004 MB |
| blink_function_call | 2000 | 131.9 s, 833 MB | 143.5 s, 1004 MB |
| interrupt | 2000 | 130.5 s, 833 MB | 144.4 s, 1004 MB |
| blink | 2000 | 132.8 s, 834 MB | 138.6 s, 1004 MB |
| blink (read_arch k6_frac mem32K, --vtr-compat, hardmap, unmap, lower) | 2000 | 153.6 s, 1169 MB | n/a |

The RAM does not map hard: the BRAM has two read and two write ports, and `hardmap` reports "no
fixed-subset mapping; left to lower" for it and the register file. The hardmap/unmap run is
therefore the same 132,446 flip-flops.

## Odin III bugs found and fixed

1. `$readmemh`/`$readmemb` took only a string literal as the file name. Now any constant string
   expression (parameter, localparam, concatenation); NUL bytes dropped as Yosys does.
2. `sim_tb`: an unsized testbench `parameter N = 3` had width 1 (read as 1), so
   `if (cycle == N) $finish;` fired on the wrong cycle or never.
3. Simulator: a clock passing through a BLIF identity buffer (`.names i o / 1 1`, which
   `write_blif` writes for a module whose output is its input, here `pll_clock`) was rejected as
   "not a primary input". The clock check now follows identity cells (`$_BUF_`, one-input identity `$lut`, `$sop` row `1 1`)
   back to the input (the cells stay in the simulation).

## Found, not fixed (both taken up in 2026-11-01-initloop-derived-clocks.md)

- `proc` and `opt` are quadratic in the length of an `initial` loop that writes a memory held as
  registers (Yosys mem2reg on blocking writes): a 128-word-per-lane RAM took 11.6 s in `proc` and
  25.5 s in `opt` (0.4 s without the loop). The zero-fill loop therefore goes to the iverilog copy
  only.
- The simulator takes clocks from primary inputs only (derived clocks and clock muxes are
  refused, as documented). `8bit_TC.v` clocks the timer from a prescaler mux; the patch runs it
  on `clock` with an enable (patch 5 of `tests/mcu/prepare.sh`).
- Quartus-specific patches (all in `tests/mcu/prepare.sh`): PLL stub, string-built INIT_FILE name
  (`{"0"+i, BASE}` has no definite width in iverilog), derived timer clock, `byte` as an
  identifier and `$time()` live only in the upstream testbench, which is not used.
