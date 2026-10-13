// dff/always_active_arst: ARST that is always on makes the FF a constant
module t(input clk, input d, input z, output reg q);
// prepass: opt_expr
  wire rst = z | 1'b1;
  always @(posedge clk or posedge rst) if (rst) q <= 1'b0; else q <= d;
endmodule
