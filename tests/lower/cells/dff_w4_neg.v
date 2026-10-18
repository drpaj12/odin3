module m(input clk, input [3:0] d, output reg [3:0] q);
  always @(negedge clk) q <= d;
endmodule
