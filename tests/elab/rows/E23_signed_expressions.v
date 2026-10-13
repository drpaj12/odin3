// E23: signed expression rules — variants: signed + signed, signed + unsigned (unsigned result), signed compare, >>> on a signed operand, a signed literal operand, $signed(u) + a
module e23_signed (input signed [3:0] a, b, input [3:0] u, output signed [7:0] y1, y2, y5, y6, output y3, output [7:0] y4);
  assign y1 = a + b;
  assign y2 = a + u;
  assign y3 = a < b;
  assign y4 = a >>> 1;
  assign y5 = 4'sb1000 + a;
  assign y6 = $signed(u) + a;
endmodule
