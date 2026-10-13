// Task 22 fix round 1: `-:` on a descending target with a signed base, low end: s = 0 and 1 write
// y[0] and y[1:0], which an unsigned base does not in Yosys 0.55 (spec 17 #54). s = -1 .. -4 are
// in the signed index's range and write nothing.
module t (input signed [2:0] s, input [2:0] d, input [3:0] a, output reg [3:0] y);
  always @* begin
    y = a;
    y[s -: 3] = d;
  end
endmodule
