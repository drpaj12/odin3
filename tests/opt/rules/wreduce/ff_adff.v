// the reset value of the removed bits is 0, the rest is kept in the narrower FF
module ff_adff(input clk, input r, input [3:0] a, input [3:0] b, output reg [7:0] q);
  always @(posedge clk or posedge r)
    if (r) q <= 8'h0a;
    else q <= a + b;
endmodule
