// dff/always_active_clr: CLR that is always on makes the bit a constant
module t(input clk, input d, input z, input o, output reg q);
// prepass: opt_expr
  wire cl = z | 1'b1;
  always @(posedge clk or posedge o or posedge cl) if (cl) q <= 1'b0; else if (o) q <= 1'b1; else q <= d;
endmodule
