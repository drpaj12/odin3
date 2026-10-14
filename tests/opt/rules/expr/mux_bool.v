// expr/mux_bool: 1-bit $mux with A = 0, B = 1 -> Y = S
// options: -mux_bool
module t(input s, output y);
  assign y = s ? 1'b1 : 1'b0;
endmodule
