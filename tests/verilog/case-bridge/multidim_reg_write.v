// A variable index into every dimension of a two-dimensional register array as the target of
// a clocked process, the array forced to registers.
module t (input clk, input [1:0] i, input [1:0] j, input [3:0] d, input [3:0] e,
          output reg [3:0] q);
  (* mem2reg *) reg [3:0] m [0:3][0:3];
  always @(posedge clk) begin
    m[i][j] <= d;
    m[1][1] <= e;
    q <= m[j][i] + m[1][1];
  end
endmodule
