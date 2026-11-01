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
#  2. iverilog only (<out-dir>/iv): the RAM is zero before $readmemh overlays the image (the
#     image has fewer words than the RAM; iverilog starts at x, the Odin III simulator at 0).
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
mkdir -p "$out/iv"
cp -r "$out/rtl" "$out/src" "$out/iv/"
sed -i 's/^    integer i;$/&\n    integer zero_k;/;
        s/^    `ifdef XILINX_SIMULATOR/    for (zero_k = 0; zero_k < 2**`MLEN\/4; zero_k = zero_k + 1) MEM[zero_k] = 32'"'"'d0;\n&/' \
    "$out/iv/rtl/darkram.v"
grep -qF "MEM[zero_k]" "$out/iv/rtl/darkram.v" || { echo "prepare.sh: patch 2 not applied" >&2; exit 2; }
echo darksocv.v darkbridge.v darkriscv.v darkram.v darkio.v darkuart.v darkpll.v
