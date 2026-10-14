// reduce/mux_bits: -fine: $mux Y bits with equal input tuples are merged
// options: -fine
module t(input s, input a, input b, output [1:0] y);
  assign y = s ? {a, a} : {b, b};
endmodule
