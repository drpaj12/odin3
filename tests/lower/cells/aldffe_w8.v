module m(input clk, input en, input ld, input [7:0] ad, input [7:0] d, output reg [7:0] q);
  always @(posedge clk or posedge ld) if (ld) q <= ad; else if (en) q <= d;
endmodule
