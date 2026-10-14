// dff/never_active_set: SET that never fires is removed from a $dffsr
module t(input clk, input d, input z, input o, output reg q);
// prepass: opt_expr
  wire st = z & 1'b0;
  always @(posedge clk or posedge st or posedge o) if (o) q <= 1'b0; else if (st) q <= 1'b1; else q <= d;
endmodule
