// Q1: n-input gates — variants: and nand or nor xor xnor with two and three inputs, buf, not, named and unnamed instances
module q1_gates (input a, b, c, output y1, y2, y3, y4, y5, y6, y7, y8);
  and g1 (y1, a, b);
  nand (y2, a, b, c);
  or g3 (y3, a, b, c);
  nor (y4, a, b);
  xor g5 (y5, a, b);
  xnor (y6, a, b, c);
  buf g7 (y7, a);
  not (y8, a);
endmodule
