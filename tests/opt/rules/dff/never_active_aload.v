// dff/never_active_aload: ALOAD that never fires is removed
module t(input clk, input d, input z, input ad, output reg q);
// prepass: opt_expr
  wire al = z & 1'b0;
  always @(posedge clk or posedge al) if (al) q <= ad; else q <= d;
endmodule
