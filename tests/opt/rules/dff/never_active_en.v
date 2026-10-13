// dff/never_active_en: EN that is never on removes the D path
module t(input clk, input d, input z, output reg q);
// prepass: opt_dff; opt_expr
  wire en = z & 1'b0;
  always @(posedge clk) if (en) q <= d;
endmodule
