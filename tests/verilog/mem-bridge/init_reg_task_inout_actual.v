// PR 2D-6 review I1: the same through an inout formal.
module t(input clk, input [7:0] a, output reg [7:0] y0);
  reg [7:0] r = 8'h5;
  task tk(inout [7:0] z, input [7:0] x); z = z + x; endtask
  always @(posedge clk) begin tk(r, a); y0 <= r; end
endmodule
