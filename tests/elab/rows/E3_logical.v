// E3: logical operators — variants: !a, a && b, a || b, !a && b (unary binds to the primary), a || b && c, a && b || c
module e3_logical (input a, b, c, output y1, y2, y3, y4, y5, y6);
  assign y1 = !a;
  assign y2 = a && b;
  assign y3 = a || b;
  assign y4 = !a && b;
  assign y5 = a || b && c;
  assign y6 = a && b || c;
endmodule
