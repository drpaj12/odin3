// L4: sized based numbers — variants: 'b 'o 'd 'h bases, upper-case base letters and digits, _ separators, x and z digits in either case, decimal x and z
module l4_sized_numbers (output [7:0] y1, y2, y3, y4, y5, y6, y7, y8, output [11:0] y9, output [15:0] y10);
  assign y1 = 8'hFF;
  assign y2 = 8'b1010_0101;
  assign y3 = 8'b10xz_XZ01;
  assign y4 = 8'hzX;
  assign y5 = 8'HfF;
  assign y6 = 8'D255;
  assign y7 = 8'dx;
  assign y8 = 8'dZ;
  assign y9 = 12'o7_7_7;
  assign y10 = 16'd65_535;
endmodule
