// E8: relational and equality — variants: < > <= >= == != with operands of different widths, a < b == c, a == b != c (left), a + b < c
module e8_compare (input [3:0] a, input [7:0] b, input c, output y1, y2, y3, y4, y5, y6, y7, y8, y9);
  assign y1 = a < b;
  assign y2 = a > b;
  assign y3 = a <= b;
  assign y4 = a >= b;
  assign y5 = a == b;
  assign y6 = a != b;
  assign y7 = a < b == c;
  assign y8 = a == b != c;
  assign y9 = a + b < c;
endmodule
