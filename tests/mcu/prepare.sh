#!/bin/sh
# prepare.sh <upstream-dir> <out-dir> <program.vmh> [mem-address-bits]
# Copies the Arduino-on-RISC-V design (quartus_project_revert_timer, the VERILOG_FILE lines of its
# .qsf) and the four byte images of <program> into <out-dir>, applies the patch set below, adds
# the PLL stub and the port-only testbench. The upstream tree is read only; nothing of it is kept
# in this repository. Prints the design file list (one line) on stdout.
#
# Patches (all in the copy):
#  1. dual_port_BRAM_byte_en.v: INIT_FILE_BASE defaults to <program>.
#  2. dual_port_BRAM_byte_en.v: {"0"+i, BASE} has no definite width for iverilog; the byte digit is
#     an 8-bit localparam of the generate block. Odin III and Yosys accept the original; the
#     patch keeps both tools on one source.
#  3. pll_clock (Altera altpll, no source) is pll_stub.v.
#  4. <out-dir>/iv/dual_port_BRAM.v (iverilog): the RAM starts as zeros before $readmemh overlays
#     the image (a Quartus M9K is zero; iverilog starts at x, the Odin III simulator at 0). Odin III
#     gets the same loop only with a mem-address-bits argument: the loop turns the RAM into
#     registers, and proc, opt and the elaboration grow with the square of the words per lane
#     (run.sh uses `proc --share-selects` then; see docs/notes/2026-11-01-initloop-derived-clocks.md).
#  5. (removed) 8bit_TC.v is unpatched: its timer clock clk_T0, a mux of the prescaler outputs, is a
#     derived clock, which the Odin III simulator takes since 2026-11-01.
#  6. optional: the default MEM_ADDRESS_BITS of single_cycle_BRAM_top (14 = 64 KB of RAM).
set -eu
up=$1; out=$2; prog=$3; bits=${4:-}
here=$(cd "$(dirname "$0")" && pwd)
src=$up/quartus_project_revert_timer
[ -d "$src" ] || { echo "prepare.sh: $src not found" >&2; exit 2; }
mkdir -p "$out"
files=$(sed -n 's/.*VERILOG_FILE \(.*\.v\).*/\1/p' "$src/single_cycle_BRAM_top.qsf")
for f in $files; do cp "$src/$f" "$out/$f"; done
for n in 0 1 2 3; do cp "$src/$n$prog" "$out/$n$prog"; done
# applied <file> <text>: exit 2 unless the patched text is in the file (a sed that matched nothing).
applied() { grep -qF -- "$2" "$1" || { echo "prepare.sh: patch not applied: $2 in $1" >&2; exit 2; }; }
sed -i "s/parameter INIT_FILE_BASE = \"[^\"]*\"/parameter INIT_FILE_BASE = \"$prog\"/" \
    "$out/dual_port_BRAM_byte_en.v"
applied "$out/dual_port_BRAM_byte_en.v" "parameter INIT_FILE_BASE = \"$prog\""
sed -i 's/\.INIT_FILE({"0"+i,INIT_FILE_BASE})/.INIT_FILE({PFX,INIT_FILE_BASE})/;
        s/^\( *if(INIT_FILE_BASE != "") begin : IF_INIT\)/\1\n    localparam [7:0] PFX = 8'"'"'d48 + i;/' \
    "$out/dual_port_BRAM_byte_en.v"
applied "$out/dual_port_BRAM_byte_en.v" "localparam [7:0] PFX = 8'd48 + i;"
applied "$out/dual_port_BRAM_byte_en.v" ".INIT_FILE({PFX,INIT_FILE_BASE})"
if [ -n "$bits" ]; then
    sed -i "s/parameter MEM_ADDRESS_BITS = 14/parameter MEM_ADDRESS_BITS = $bits/" \
        "$out/single_cycle_BRAM_top.v"
    applied "$out/single_cycle_BRAM_top.v" "parameter MEM_ADDRESS_BITS = $bits"
fi
cp "$here/pll_stub.v" "$here/mcu_tb.v" "$out/"
mkdir -p "$out/iv"
cp "$out"/*.v "$out"/*.vmh "$out/iv/"
sed -i 's/^localparam RAM_DEPTH = 1 << ADDR_WIDTH;/&\ninteger zero_k;/;
        s/^  if(INIT_FILE != "")/  for (zero_k = 0; zero_k < RAM_DEPTH; zero_k = zero_k + 1) ram[zero_k] = {DATA_WIDTH{1'"'"'b0}};\n&/' \
    "$out/iv/dual_port_BRAM.v"
applied "$out/iv/dual_port_BRAM.v" "ram[zero_k] = "
applied "$out/iv/dual_port_BRAM.v" "integer zero_k;"
if [ -n "$bits" ]; then
    cp "$out/iv/dual_port_BRAM.v" "$out/dual_port_BRAM.v"
fi
echo $files pll_stub.v
