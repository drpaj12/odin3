module m(input clk, input rst, input [7:0] d, output reg [7:0] q);
  always @(posedge clk) if (rst) q <= 8'hA5; else q <= d;
endmodule
