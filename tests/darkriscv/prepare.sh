#!/bin/sh
# prepare.sh <upstream-dir> <out-dir>
# Copies the DarkRISCV SoC (rtl/*.v, src/darksocv.mem) into <out-dir>/{rtl,src} (the RTL
# includes ../rtl/config.vh and reads ../src/darksocv.mem) and adds the port-only testbench. The
# upstream tree is read only; nothing of it is kept in this repository. Prints the design file
# list (one line, relative to <out-dir>/rtl) on stdout.
#
# Patches (all in the copy):
#  1. __ICARUS__ is renamed: iverilog always defines it, which turns on SIMULATION ($write,
#     $fflush, $finish on the '>' prompt, a RAM zero loop). Odin III does not define it. Both tools
#     see the non-SIMULATION design.
#  2. The UART speed is raised from 115200 to 100000000 bit/s (a `define before the `ifndef in
#     config.vh): the baud divisor drops from 868 to 1 (a bit lasts 2 clocks), so the logo and
#     the title line fit in 36000 cycles. Both tools see the same copy.
set -eu
up=$1; out=$2
here=$(cd "$(dirname "$0")" && pwd)
[ -f "$up/src/darksocv.mem" ] || { echo "prepare.sh: $up/src/darksocv.mem not found" >&2; exit 2; }
mkdir -p "$out/rtl" "$out/src"
cp "$up"/rtl/*.v "$up/rtl/config.vh" "$out/rtl/"
cp "$up/src/darksocv.mem" "$out/src/"
sed -i 's/__ICARUS__/__ICARUS_OFF__/g' "$out"/rtl/*.v "$out/rtl/config.vh"
grep -q __ICARUS_OFF__ "$out/rtl/config.vh" || { echo "prepare.sh: patch 1 not applied" >&2; exit 2; }
cp "$here/dark_tb.v" "$out/rtl/"
sed -i 's/^`ifndef __UARTSPEED__$/`define __UARTSPEED__ 100000000\n&/' "$out/rtl/config.vh"
grep -q '^`define __UARTSPEED__ 100000000$' "$out/rtl/config.vh" || { echo "prepare.sh: patch 2 not applied" >&2; exit 2; }
mkdir -p "$out/iv"
cp -r "$out/rtl" "$out/src" "$out/iv/"
echo darksocv.v darkbridge.v darkriscv.v darkram.v darkio.v darkuart.v darkpll.v
