// expr/pow_shift: $pow with constant 2 -> $shl(1, B)
module t(input [3:0] b, output [7:0] y);
  wire [7:0] k = 8'd2;
  assign y = k ** b;
endmodule
