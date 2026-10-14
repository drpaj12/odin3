// the top 4 bits of the register are never read
module ff_unused(input clk, input [7:0] d, output [3:0] y);
  reg [7:0] q;
  always @(posedge clk) q <= d;
  assign y = q[3:0];
endmodule
