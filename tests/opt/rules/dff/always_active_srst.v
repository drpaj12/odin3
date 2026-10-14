// dff/always_active_srst: SRST that is always on makes D constant
module t(input clk, input d, input z, output reg q);
// prepass: opt_dff; opt_expr
  wire r = z | 1'b1;
  always @(posedge clk) if (r) q <= 1'b0; else q <= d;
endmodule
