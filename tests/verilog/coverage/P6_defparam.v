// P6: defparam — variants: one level u1.W, nested u_mid.u_leaf.W, two pairs in one statement, a generate-scoped target g[0].u.W, a defparam inside a generate block
module p6_leaf (input [7:0] a, output [7:0] y);
  parameter W = 1;
  assign y = a + W;
endmodule

module p6_mid (input [7:0] a, output [7:0] y);
  p6_leaf u_leaf (.a(a), .y(y));
endmodule

module p6_defparam (input [7:0] a, output [7:0] y1, y2, output [15:0] y3);
  p6_leaf u1 (.a(a), .y(y1));
  p6_mid u_mid (.a(a), .y(y2));
  defparam u1.W = 2;
  defparam u_mid.u_leaf.W = 3, u1.W = 4;
  genvar i;
  for (i = 0; i < 2; i = i + 1) begin : g
    p6_leaf u (.a(a), .y(y3[8 * i +: 8]));
  end
  defparam g[0].u.W = 5;
  generate
    for (i = 0; i < 1; i = i + 1) begin : h
      p6_leaf u (.a(a), .y());
      defparam u.W = 6;
    end
  endgenerate
endmodule
