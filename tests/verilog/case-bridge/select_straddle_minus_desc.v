// Task 22 fix round 1: `-:` on a descending target, high end: y[b -: 3] with b = {1'b1, s} (4 to 7)
// covers b .. b-2, so 4 and 5 straddle the top of [3:0] and write y[3] and y[3:2]. The low end of
// an unsigned `-:` base is the one divergence from Yosys 0.55 (spec 17 #54), kept out of here.
module t (input [1:0] s, input [2:0] d, input [3:0] a, output reg [3:0] y);
  always @* begin
    y = a;
    y[{1'b1, s} -: 3] = d;
  end
endmodule
