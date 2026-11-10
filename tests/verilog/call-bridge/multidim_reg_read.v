// A variable index into a two-dimensional register array written with constant indices (an
// unrolled loop), with a descending first dimension.
module t (input clk, input [1:0] i, input j, input [3:0] d, output reg [3:0] q, output [3:0] y);
  reg [3:0] m [3:0][0:1];
  integer p, k;
  always @(posedge clk) begin
    for (p = 0; p < 4; p = p + 1)
      for (k = 0; k < 2; k = k + 1)
        m[p][k] <= d + p * 2 + k;
    q <= m[i][j];
  end
  assign y = m[i][j];
endmodule
