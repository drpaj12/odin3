# Plan: DarkRISCV under Odin III testbench simulation (2026-11-01)

Purpose: second Seeker-Proc v2.0 base for Leman Russ III, same method as the Arduino-on-RISC-V
microcontroller (docs/notes/2026-10-31-mcu-testbench.md). Measure: Odin III runs a real RV32I
SoC and its prebuilt firmware (`src/darksocv.mem`), on the RTL and on the netlist, matching
iverilog cycle for cycle on the ports.

Design: github.com/darklife/darkriscv (BSD-3-Clause), commit 9555119, read-only clone outside
this repo. Nothing of it is vendored; the test skips when the clone is absent.

DUT boundary: `darksocv` (core, bridge, RAM with `$readmemh`, UART, IO block, PLL/reset block).
The testbench (`tests/darkriscv/dark_tb.v`) drives XCLK and XRES and records, at every falling
clock edge, the cycle number, UART_TXD, LED, OPORT and DEBUG. The core bus is internal to the
SoC and not a port, so it is not observed; the UART line carries the program's output.

Steps
1. `prepare.sh` copies rtl/*.v and src/darksocv.mem into a scratch tree with the same
   `rtl/` and `src/` layout (the RTL includes `../rtl/config.vh` and reads `../src/darksocv.mem`),
   neutralises the iverilog-only `__ICARUS__` branches so both tools see one source, and gives
   iverilog a zero-filled RAM (a copy only).
2. iverilog reference; `compare.py` rules as for the microcontroller (x masked only in the reset
   window; exit 2 on short or empty traces).
3. sim_tb on the RTL (proc; opt; lower) and on the netlist (write_blif, read back); release
   build for timing. Report cycles, wall, memory, DFFs, masked fields, first mismatch.
4. Fix only Odin III bugs found, each with a regression micro.
5. CTest `tb_darkriscv` (CMake cache variable `ODIN3_DARKRISCV_UPSTREAM`, skips with 77).
