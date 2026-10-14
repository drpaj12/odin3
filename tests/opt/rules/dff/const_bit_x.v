// dff/const_bit_x: D = 1'bx with no init: the bit becomes constant x (Yosys prints 1-bit for it)
module t(input clk, input d, input z, output reg q);
// prepass:
  always @(posedge clk) q <= 1'bx;
endmodule
