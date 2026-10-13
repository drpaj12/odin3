// G7: generate-scope names and references — variants: g[0].t and g[3].t into a named loop block, genblk2.w into an unnamed if-generate block (implicit name)
module g7_scope_names (input [3:0] a, output y1, y2, y3);
  genvar i;
  for (i = 0; i < 4; i = i + 1) begin : g
    wire t;
    assign t = a[i];
  end
  if (1) begin
    wire w;
    assign w = a[0];
  end
  assign y1 = g[0].t;
  assign y2 = g[3].t;
  assign y3 = genblk2.w;
endmodule
