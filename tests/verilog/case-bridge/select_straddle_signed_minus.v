// Task 22 fix round 1: a signed base with `-:` on an ascending target: s = -1 covers -1 .. -3 and
// writes nothing; s = 0 covers 0 .. -2 and writes y[0]; s = 3 covers 3 .. 1 (inside).
module t (input signed [2:0] s, input [2:0] d, input [3:0] a, output reg [0:3] y);
  always @* begin
    y = a;
    y[s -: 3] = d;
  end
endmodule
