// dff/d_eq_q: D = Q: the FF holds its value
module t(input clk, input d, input z, output reg q);
// prepass: opt_expr
  always @(posedge clk) q <= q;
endmodule
