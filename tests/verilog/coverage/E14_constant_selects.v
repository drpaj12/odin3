// E14: constant bit- and part-selects — variants: a[0], a[7:4], a[2 + 1] (a constant expression index), bit and part selects as assign targets
module e14_const_select (input [7:0] a, output y1, output [3:0] y2, output y3, output [7:0] y4);
  assign y1 = a[0];
  assign y2 = a[7:4];
  assign y3 = a[2 + 1];
  assign y4[1:0] = a[1:0];
  assign y4[2] = a[3];
endmodule
