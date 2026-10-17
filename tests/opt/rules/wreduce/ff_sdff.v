module ff_sdff(input clk, input r, input [3:0] a, input [3:0] b, output reg [7:0] q);
  always @(posedge clk)
    if (r) q <= 8'h05;
    else q <= a + b;
endmodule
