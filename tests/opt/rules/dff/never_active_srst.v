// dff/never_active_srst: SRST that never fires is removed
module t(input clk, input d, input z, output reg q);
// prepass: opt_dff; opt_expr
  wire r = z & 1'b0;
  always @(posedge clk) if (r) q <= 1'b0; else q <= d;
endmodule
