// expr/mul_shift.swapped: $mul with a one-hot constant A -> $shl of B
module t(input [3:0] a, output [3:0] y);
  wire [3:0] k = 4'd4;
  assign y = a * k;
endmodule
