// dff/const_clk: a constant clock removes the D path
module t(input clk, input d, input z, output reg q);
// prepass: opt_expr
  wire c = z & 1'b0;
  always @(posedge c) q <= d;
endmodule
