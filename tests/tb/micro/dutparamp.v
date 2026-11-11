module dutparamp #(parameter W = 8, parameter N = 3, parameter NAME = "ab")
  (input clk, input [W-1:0] a, output reg [W-1:0] q, output [7:0] first);
  assign first = NAME[15:8];
  always @(posedge clk) q <= a + N;
endmodule
