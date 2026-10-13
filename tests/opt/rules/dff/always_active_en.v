// dff/always_active_en: EN that is always on is removed
module t(input clk, input d, input z, output reg q);
// prepass: opt_dff; opt_expr
  wire en = z | 1'b1;
  always @(posedge clk) if (en) q <= d;
endmodule
