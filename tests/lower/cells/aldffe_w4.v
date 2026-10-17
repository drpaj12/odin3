module m(input clk, input en, input ld, input [3:0] ad, input [3:0] d, output reg [3:0] q);
  always @(posedge clk or posedge ld) if (ld) q <= ad; else if (en) q <= d;
endmodule
