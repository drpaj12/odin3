// E6: general / and % — variants: a / b, a % b, a / 3, a % 3, a - b % c
module e6_div_mod (input [7:0] a, b, c, output [7:0] y1, y2, y3, y4, y5);
  assign y1 = a / b;
  assign y2 = a % b;
  assign y3 = a / 3;
  assign y4 = a % 3;
  assign y5 = a - b % c;
endmodule
