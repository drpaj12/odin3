module dutparams #(parameter W = 8, parameter N = 3, parameter [15:0] TAG = 16'h1234,
                   parameter NAME = "ab")
  (input clk, input [W-1:0] a, output reg [W-1:0] q, output [15:0] tag);
  localparam K = 1;
  assign tag = TAG;
  always @(posedge clk) q <= a + N + K;
endmodule
