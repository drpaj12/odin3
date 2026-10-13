// Task 22 fix round 1: `-:` on an ascending target [0:7], right end: b = {1'b1, s} (8 to 11) covers
// b-2 .. b, so 8 writes y[6:7] and 9 writes y[7]. The low end of an unsigned `-:` base is spec
// 17 #54.
module t (input [1:0] s, input [2:0] d, input [7:0] a, output reg [0:7] y);
  always @* begin
    y = a;
    y[{1'b1, s} -: 3] = d;
  end
endmodule
