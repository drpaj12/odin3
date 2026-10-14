// dff/add_srst: a $mux with a constant on D becomes a synchronous reset
module t(input clk, input d, input r, output reg q);
// prepass: 
  always @(posedge clk) if (r) q <= 1'b0; else q <= d;
endmodule
