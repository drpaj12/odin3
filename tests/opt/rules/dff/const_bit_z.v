// dff/const_bit_z: D = 1'bz with no init: the bit becomes constant z (Yosys prints 1-bit for it)
module t(input clk, input d, input z, output reg q);
// prepass:
  always @(posedge clk) q <= 1'bz;
endmodule
