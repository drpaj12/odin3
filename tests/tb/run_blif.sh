#!/bin/sh
# run_blif.sh <odin3>: a clock that passes through a BLIF buffer model (`.names i o / 1 1`, as
# write_blif writes a module whose output is its input) is still the primary input: sim_tb on the
# netlist must run and give the same $display text as iverilog on the RTL.
set -u
odin3=$1
work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-tbblif.XXXXXX")
cd "$work" || exit 2
cat >top.v <<'V'
module bufm(input i, output o); assign o = i; endmodule
module top(input clk, input d, output reg q);
  wire c;
  bufm b(.i(clk), .o(c));
  always @(posedge c) q <= d;
endmodule
V
cat >tb.v <<'V'
module tb;
  reg clk, d; wire q;
  top dut(.clk(clk), .d(d), .q(q));
  initial begin
    clk = 0; d = 1;
    #5 clk = 1;
    #5 $display("q=%b", q);
    d = 0; clk = 0;
    #5 clk = 1;
    #5 $display("q=%b", q);
    $finish;
  end
endmodule
V
if ! "$odin3" --top top -p "read_verilog top.v; proc; opt; lower; write_blif top.blif" >build.log 2>&1; then
    echo "FAIL blif build"; tail -3 build.log; exit 1
fi
if ! "$odin3" --top top -p "read_blif top.blif; sim_tb --out out.txt tb.v" >run.log 2>&1; then
    echo "FAIL blif sim_tb"; grep error run.log | head -3; exit 1
fi
printf 'q=1\nq=0\n' >want.txt
if diff -u want.txt out.txt; then echo "PASS blif_clock_through_buffer"; else echo "FAIL blif_clock_through_buffer"; exit 1; fi
