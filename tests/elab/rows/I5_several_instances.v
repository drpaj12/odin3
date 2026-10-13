// I5: several instances in one statement — variants: two named instances, one with a parameter override applying to both
module i5_leaf #(parameter W = 1) (input [3:0] a, output [3:0] y);
  assign y = a + W;
endmodule

module i5_several (input [3:0] a, b, output [3:0] y1, y2, y3, y4);
  i5_leaf u1 (.a(a), .y(y1)), u2 (.a(b), .y(y2));
  i5_leaf #(2) u3 (a, y3), u4 (b, y4);
endmodule
