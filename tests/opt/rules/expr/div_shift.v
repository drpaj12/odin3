// expr/div_shift: $div by a power of two -> $shr
module t(input [3:0] a, output [3:0] y);
  wire [3:0] k = 4'd4;
  assign y = a / k;
endmodule
