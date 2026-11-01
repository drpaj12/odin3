# Plan: DarkRISCV under Odin III testbench simulation (2026-11-01)

Purpose: second Seeker-Proc v2.0 base for Leman Russ III, same method as the Arduino-on-RISC-V
microcontroller (docs/notes/2026-10-31-mcu-testbench.md). Measure: Odin III runs a real RV32I
SoC and its prebuilt firmware (`src/darksocv.mem`), on the RTL and on the netlist, matching
iverilog cycle for cycle on the ports.

Design: github.com/darklife/darkriscv (BSD-3-Clause), commit 9555119, read-only clone outside
this repo. Nothing of it is vendored; the test skips when the clone is absent.

DUT boundary: `darksocv` (core, bridge, RAM with `$readmemh`, UART, IO block, PLL/reset block).
The testbench (`tests/darkriscv/dark_tb.v`) drives XCLK and XRES and records, at every falling
clock edge, the cycle number, UART_TXD, LED, OPORT and DEBUG[3:2] (bus request, halt). A UART
receiver in the testbench prints one line per received byte. The core bus is internal to the SoC
and not a port, so it is not observed; the UART text carries the program's output.

Steps
1. `prepare.sh` copies rtl/*.v and src/darksocv.mem into a scratch tree with the same
   `rtl/` and `src/` layout (the RTL includes `../rtl/config.vh` and reads `../src/darksocv.mem`),
   neutralises the iverilog-only `__ICARUS__` branches so both tools see one source, and raises
   the UART speed to 100 Mbit/s in the copy (divisor 1, a bit lasts 2 clocks; at 115200 the
   divisor is 868 and 5000 cycles show one character).
2. iverilog reference; `compare.py` rules as for the microcontroller (x masked only in the reset
   window; exit 2 on short or empty traces); `uart_text.py` compares the UART lines (cycle and
   byte) of the two runs and the decoded text with `tests/darkriscv/expect.txt`.
3. sim_tb on the RTL (proc; opt; lower) and on the netlist (write_blif, read back); release
   build for timing. Report cycles, wall, memory, DFFs, masked fields, first mismatch.
4. Fix only Odin III bugs found, each with a regression micro.
5. CTest `tb_darkriscv` (6000 cycles) and sweep `tb_darkriscv_full` (36000 cycles); CMake cache
   variable `ODIN3_DARKRISCV_UPSTREAM`, the environment variable wins when set; skips with 77 unless
   the checkout is at commit 9555119.

## Result

Design `darksocv`: 8 KB RAM as soft logic, lowered 67,638 `$_DFF_P_` (65,536 RAM bits, 2,102
others). Trace per cycle: UART_TXD, LED, OPORT, DEBUG[3] (bus request), DEBUG[2] (halt); plus one
`UART <cycle> <hex> <char>` line per byte received. DEBUG[1] is x in iverilog and DEBUG[0] (the
instruction-ack toggle) is left out.

The firmware prints its logo (rows of spaces, `v` and `r`) and the title line, then the boot
messages. At the stock 115200 bit/s the divisor is 868, 5000 cycles show the first four bits of
one `\n` (24 distinct fetch addresses) and 120000 cycles 38 addresses. `prepare.sh` raises the
speed to 100 Mbit/s in the copy (divisor 1): the title line is complete at cycle 35500.

| run | cycles | distinct fetch addresses (iverilog) | decoded UART | release wall, peak memory |
|---|---|---|---|---|
| tb_darkriscv | 6000 | 40 | 167 bytes, the first four logo rows | RTL 87 s, 178 MB; netlist 87 s, 272 MB |
| tb_darkriscv_full (sweep) | 36000 | 130 | 1065 bytes, the whole logo and `INSTRUCTION SETS WANT TO BE FREE` | RTL 548 s, 178 MB; netlist 489 s, 272 MB |

iverilog 0.19 s and 1.4 s, 8 MB. Odin III RTL and netlist: every trace line and every UART line
(cycle and byte) identical to iverilog; the decoded text equals `tests/darkriscv/expect.txt` (the
first 23 lines of output, trailing spaces ignored). Masked fields: 0. Cost: about 14 ms per cycle in
Release (other jobs were running); about 90 ms per cycle in Debug.

Sensitivity (iverilog only, a copy of `darkriscv.v` with one change, 6000 cycles): a stuck bit 0 of
the xor operand, a shift right losing bit 0, and a cleared bit 4 of the adder result all change
the UART text; an AND with bit 0 forced to 1 and a shift left with bit 1 forced to 1 do not change
it in 6000 or 36000 cycles, so those two are not covered.

## Odin III bugs found and fixed

None. The first full flow ran identical to iverilog.

## Found, not fixed

- DEBUG[1] (data ack) is x in iverilog on every UART access (an uninitialised value in
  darkio); it is not recorded.
- iverilog defines `__ICARUS__`, which turns on the upstream `SIMULATION` code (`$write`,
  `$fflush`, `$finish` on the `>` prompt); `prepare.sh` renames it so both tools see the
  non-SIMULATION design. Upstream-specific, logged as patch 1.
- `check` warns "rule 4: net has no pins" for 54+ unnamed nets of `darkriscv` after `opt`
 ; cause not investigated, results are unaffected.

## MCU test coverage (tests/mcu, checked 2026-11-01)

Distinct PC values after reset, from iverilog: blink_interrupt 138 at 500 cycles and still 138 at
6000 (a delay loop; PORTB stays 0, PORTD changes once, to 0x20, at cycle 234); blink 393 and
blink_function_call 80 at 2000, interrupt 163 at 2000 (PORTB changes once). The 500-cycle ctest
therefore covers the whole of blink_interrupt that is reachable, but a stuck-at-0 bit of PORTB or
of PORTD other than bit 5 is invisible there, and the PC port shows only control-flow errors.
`tb_mcu_blink` and `tb_mcu_interrupt` (2000 cycles) add the other programs, where PORTB and PORTD
each change.
