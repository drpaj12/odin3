// E5: / and % by a power-of-2 constant — variants: a / 4, a % 4, a / 8'd8, a * b / 4 % 2 (one precedence level, left), a + b / 4
module e5_div_mod_pow2 (input [7:0] a, b, output [7:0] y1, y2, y3, y4, y5);
  assign y1 = a / 4;
  assign y2 = a % 4;
  assign y3 = a / 8'd8;
  assign y4 = a * b / 4 % 2;
  assign y5 = a + b / 4;
endmodule
