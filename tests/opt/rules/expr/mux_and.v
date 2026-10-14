// expr/mux_and: 1-bit $mux with A = 0 -> $and(S, B)
// options: -mux_bool
module t(input s, input b, output y);
  assign y = s ? b : 1'b0;
endmodule
