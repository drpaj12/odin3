// Task 22 fix round 1: `+:` on an ascending target [0:7]: the declared indices count up from the
// left, so the slice at s covers s .. s+2 and straddles the right end for s = 6, 7.
module t (input [2:0] s, input [2:0] d, input [7:0] a, output reg [0:7] y);
  always @* begin
    y = a;
    y[s +: 3] = d;
  end
endmodule
