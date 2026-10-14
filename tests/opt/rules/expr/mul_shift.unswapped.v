// expr/mul_shift.unswapped: $mul with a one-hot constant B -> $shl of A
module t(input [3:0] a, output [3:0] y);
  wire [3:0] k = 4'd4;
  assign y = k * a;
endmodule
