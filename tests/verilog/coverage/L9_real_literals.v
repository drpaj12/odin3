// L9: real literals — variants: 1.5 and 1e3 forms (2.5e1, 1E-1, 1_000.0_1) in an expression (REJECT) and in delays (IGNORE): #1.5, #(2.5e1, 1E-1)
module l9_real_literals (input a, output y1, y2, output [7:0] y3, y4);
  assign #1.5 y1 = a;
  assign #(2.5e1, 1E-1) y2 = a;
  assign y3 = 1.5;
  assign y4 = 1e3;
  wire [7:0] w = 1_000.0_1;
endmodule
