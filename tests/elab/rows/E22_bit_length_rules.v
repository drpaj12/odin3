// E22: expression bit-length rules — variants: (a + b) >> 1 into 8 bits, (0 + a + b) >> 1, a + b into 9 bits, {a + b} >> 1 (self-determined concatenation operand)
module e22_widths (input [7:0] a, b, output [7:0] sum1, sum2, sum3, output [8:0] wide);
  assign sum1 = (a + b) >> 1;
  assign sum2 = (0 + a + b) >> 1;
  assign wide = a + b;
  assign sum3 = {a + b} >> 1;
endmodule
