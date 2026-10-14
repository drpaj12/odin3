// expr/mux_same: $mux with equal data inputs -> Y = A
module t(input s, input [3:0] a, output [3:0] y);
  assign y = s ? a : a;
endmodule
