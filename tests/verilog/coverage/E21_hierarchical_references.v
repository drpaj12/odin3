// E21: hierarchical references in expressions (REJECT) — variants: u.w, u.w[1] (a select on the last component wraps the name)
module e21_sub (input [3:0] d, output [3:0] q);
  wire [3:0] w;
  assign w = d;
  assign q = w;
endmodule

module e21_hier_ref (input [3:0] d, output [3:0] y1, output y2);
  e21_sub u (.d(d), .q());
  assign y1 = u.w;
  assign y2 = u.w[1];
endmodule
