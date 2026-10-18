module m(input clk, input ld, input [3:0] ad, input [3:0] d, output reg [3:0] q);
  always @(posedge clk or negedge ld) if (!ld) q <= ad; else q <= d;
endmodule
