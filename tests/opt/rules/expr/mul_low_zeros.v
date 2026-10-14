// expr/mul_low_zeros: $mul with a constant with low zero bits -> narrower $mul
module t(input [3:0] a, output [3:0] y);
  wire [3:0] k = 4'd6;
  assign y = a * k;
endmodule
