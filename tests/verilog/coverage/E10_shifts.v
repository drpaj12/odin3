// E10: shifts — variants: << >> <<< >>> by a constant, << and >> by a variable amount, >>> on a signed operand, a + b << 1, a << n + 1, a << 1 < b
module e10_shifts (input [7:0] a, b, input signed [7:0] s, input [2:0] n, output [7:0] y1, y2, y3, y4, y5, y6, y7, y8, y9, output y10);
  assign y1 = a << 2;
  assign y2 = a >> 2;
  assign y3 = a <<< 2;
  assign y4 = s >>> 2;
  assign y5 = a << n;
  assign y6 = a >> n;
  assign y7 = s >>> n;
  assign y8 = a + b << 1;
  assign y9 = a << n + 1;
  assign y10 = a << 1 < b;
endmodule
