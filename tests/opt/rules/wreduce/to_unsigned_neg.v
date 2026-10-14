module to_unsigned_neg(input [6:0] a, output signed [7:0] y);
  assign y = -$signed({1'b0, a});
endmodule
