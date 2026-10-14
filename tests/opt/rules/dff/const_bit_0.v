// dff/const_bit_0: D = 0 equals init 0: the bit becomes constant 0
module t(input clk, input d, input z, output reg q);
// prepass: 
  initial q = 1'b0;
  always @(posedge clk) q <= 1'b0;
endmodule
