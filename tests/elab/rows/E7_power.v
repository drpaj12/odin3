// E7: ** — variants: constant operands 2 ** 3, variable exponent a ** b and 2 ** b, a ** b ** c (left), -a ** b = (-a) ** b, a * b ** c
module e7_power (input [3:0] a, b, c, output [15:0] y1, y2, y3, y4, y5, y6);
  assign y1 = 2 ** 3;
  assign y2 = a ** b;
  assign y3 = 2 ** b;
  assign y4 = a ** b ** c;
  assign y5 = -a ** b;
  assign y6 = a * b ** c;
endmodule
