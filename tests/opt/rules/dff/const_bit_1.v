// dff/const_bit_1: D = 1 equals init 1: the bit becomes constant 1
module t(input clk, input d, input z, output reg q);
// prepass: 
  initial q = 1'b1;
  always @(posedge clk) q <= 1'b1;
endmodule
