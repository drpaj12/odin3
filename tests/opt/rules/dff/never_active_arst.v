// dff/never_active_arst: ARST that never fires is removed
module t(input clk, input d, input z, output reg q);
// prepass: opt_expr
  wire rst = z & 1'b0;
  always @(posedge clk or posedge rst) if (rst) q <= 1'b0; else q <= d;
endmodule
