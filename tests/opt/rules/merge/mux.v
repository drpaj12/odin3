// merge/mux: two identical $mux cells -> one
module t(input s, input [3:0] a, input [3:0] b, output [3:0] y, output [3:0] z);
  assign y = s ? a : b;
  assign z = s ? a : b;
endmodule
