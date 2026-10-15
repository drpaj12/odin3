// a signed cell with a 0 on top of both operands is unsigned
module to_unsigned(input [6:0] a, input [6:0] b, output signed [7:0] y);
  assign y = $signed({1'b0, a}) + $signed({1'b0, b});
endmodule
