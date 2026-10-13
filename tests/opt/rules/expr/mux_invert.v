// expr/mux_invert: 1-bit $mux with A = 1, B = 0 -> $not of S
// options: -mux_bool
module t(input s, output y);
  assign y = s ? 1'b0 : 1'b1;
endmodule
