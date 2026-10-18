module m(input clk, input rst, input [3:0] d, output reg [3:0] q);
  always @(posedge clk) if (rst) q <= 4'd5; else q <= d;
endmodule
