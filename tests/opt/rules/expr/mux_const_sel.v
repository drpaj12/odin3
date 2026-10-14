// expr/mux_const_sel: $mux with a constant select -> the selected input
module t(input [3:0] a, input [3:0] b, output [3:0] y);
  wire s = 1'b1;
  assign y = s ? a : b;
endmodule
