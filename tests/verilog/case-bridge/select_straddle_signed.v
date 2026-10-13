// Task 22 fix round 1: a signed base. s = -1 with `+: 2` writes y[0] (the slice covers -1 and 0);
// s = -2 and -3 are in the signed index's range and write nothing.
module t (input signed [2:0] s, input [1:0] d, input [3:0] a, output reg [3:0] y);
  always @* begin
    y = a;
    y[s +: 2] = d;
  end
endmodule
