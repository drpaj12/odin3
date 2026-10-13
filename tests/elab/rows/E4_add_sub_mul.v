// E4: + - * and unary - + — variants: a + b, a - b, a * b, -a, +a, a + b * c, a - b - c (left), -a * b
module e4_arith (input [7:0] a, b, c, output [7:0] y1, y2, y3, y4, y5, y6, y7, y8);
  assign y1 = a + b;
  assign y2 = a - b;
  assign y3 = a * b;
  assign y4 = -a;
  assign y5 = +a;
  assign y6 = a + b * c;
  assign y7 = a - b - c;
  assign y8 = -a * b;
endmodule
