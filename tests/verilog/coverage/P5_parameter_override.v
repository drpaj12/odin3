// P5: instance parameter overrides — variants: ordered #(8), ordered #(8, 3), named #(.W(8)), named with an empty value #(.W(8), .D())
module p5_leaf #(parameter W = 4, D = 2) (input [W-1:0] a, output [W-1:0] y);
  assign y = a;
endmodule

module p5_override (input [7:0] a, output [7:0] y1, y2, y3, y4);
  p5_leaf #(8) u1 (.a(a), .y(y1));
  p5_leaf #(8, 3) u2 (.a(a), .y(y2));
  p5_leaf #(.W(8)) u3 (.a(a), .y(y3));
  p5_leaf #(.W(8), .D()) u4 (.a(a), .y(y4));
endmodule
