// A variable index into every dimension of a two-dimensional net array: a combinational read, the
// same read inline in a clocked process, and a mix of a constant and a variable index.
module t (input clk, input [1:0] i, input [1:0] j, input [3:0] a, input [3:0] b,
          output [3:0] y, output [3:0] z, output reg [3:0] q);
  wire [3:0] u [0:3][0:3];
  genvar gi, gj;
  generate for (gi = 0; gi < 4; gi = gi + 1) begin : r
    for (gj = 0; gj < 4; gj = gj + 1) begin : c
      assign u[gi][gj] = (a + gi) ^ (b + 2 * gj);
    end
  end endgenerate
  assign y = u[i][j];
  assign z = u[3][j];
  always @(posedge clk) q <= u[i][j] ^ u[1][1];
endmodule
