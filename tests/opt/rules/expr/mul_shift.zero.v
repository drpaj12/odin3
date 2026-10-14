// expr/mul_shift.zero: $mul by constant 0 -> 0
module t(input [3:0] a, output [3:0] y);
  wire [3:0] z = 4'd0;
  assign y = a * z;
endmodule
