// PR 2D-6 review I7: the edge event reads a word of a register array with a variable index (a continuous-context build while the ALWAYS is open).
module t(input clk, input [1:0] s, input d, output reg q);
  reg [3:0] m [0:3];
  integer i;
  always @* for (i = 0; i < 4; i = i + 1) m[i] = {3'b000, clk};
  always @(posedge m[s][0]) q <= d;
endmodule
