// expr/mux_or: 1-bit $mux with B = 1 -> $or(A, S)
// options: -mux_bool
module t(input s, input a, output y);
  assign y = s ? 1'b1 : a;
endmodule
