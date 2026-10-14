// expr/constshift: $shl by a constant -> wiring
module t(input [3:0] a, output [3:0] y);
  assign y = a << 2;
endmodule
