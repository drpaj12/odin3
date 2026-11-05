`timescale 1ns / 1ps
`default_nettype none
`define WIDTH 8
`define TWICE(x) ((x) * 2)
`include "directives_inc.vh"
`ifdef NOT_DEFINED
  `define PICK 111
`else
  `define PICK 222
`endif
`celldefine
module directives_tb;
  reg [`WIDTH-1:0] r;
  initial begin
    r = `TWICE(21);
    $display("r=%0d pick=%0d inc=%0d", r, `PICK, `INC_VALUE);
    #3 $display("t=%0t", $time);
    $finish;
  end
endmodule
`endcelldefine
`resetall
