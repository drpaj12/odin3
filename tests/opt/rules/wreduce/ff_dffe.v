module ff_dffe(input clk, input en, input [3:0] a, input [3:0] b, output reg [7:0] q);
  always @(posedge clk) if (en) q <= a + b;
endmodule
