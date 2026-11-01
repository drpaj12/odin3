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

## Result (12000 cycles = 120 us; machine shared with other jobs)

Design `darksocv`: 8 KB RAM as soft logic, lowered 67,638 `$_DFF_P_` (65,536 RAM bits, 2,102
others). Trace: cycle, UART_TXD, LED, OPORT, DEBUG[3], DEBUG[2], DEBUG[0]. Firmware
`src/darksocv.mem`; the UART line shows the reset window, then four start/stop edges (cycles
380, 2118, 2987, 3856, 4725 ...), and the bus handshake bits toggle (first at cycle 4725).
iverilog reference 0.35 to 0.47 s, 8 MB. Masked fields: 0 (iverilog has no x on the recorded
signals). First mismatch: none; every line identical.

| run | debug (ASan + UBSan) | release |
|---|---|---|
| RTL (proc; opt; lower) | 594 s, 495 MB | 160 s, 178 MB |
| netlist (write_blif, read_blif) | 607 s, 596 MB | 156 s, 274 MB |

The recorded signals are only TX, LED, OPORT and three handshake bits; LED and OPORT stay 0
in the window, so the program's effect is visible through the UART and the handshakes. The
core bus is not a port.

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
- sim_tb costs about 13 ms per cycle in release for this design (about
  50 ms per cycle in debug).
