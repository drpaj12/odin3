// the sum of two 4-bit operands in an 8-bit register: wreduce makes the top bits of D 0 (round 1),
// then the register narrows (round 2)
module ff_zero_ext(input clk, input [3:0] a, input [3:0] b, output reg [7:0] q);
  always @(posedge clk) q <= a + b;
endmodule
