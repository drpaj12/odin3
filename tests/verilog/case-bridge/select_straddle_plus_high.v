// Task 22 fix round 1: a variable indexed part select that straddles the top of the target writes
// its in-range bits (IEEE 1364-2005 4.2.1; Yosys 0.55 does the same). `y[b +: 2]` with a 2-bit b
// at b = 3 writes y[3] only; a descending [7:0] target. No residual divergence.
module t (input [1:0] b, input [1:0] d, input [3:0] a, output reg [3:0] y);
  always @* begin
    y = a;
    y[b +: 2] = d;
  end
endmodule
