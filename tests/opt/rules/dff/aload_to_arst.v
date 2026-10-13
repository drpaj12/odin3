// dff/aload_to_arst: ALOAD whose AD is constant after opt_expr becomes ARST
module t(input clk, input d, input z, input al, output reg q);
// prepass: opt_expr
  wire ad = z & 1'b0;
  always @(posedge clk or posedge al) if (al) q <= ad; else q <= d;
endmodule
