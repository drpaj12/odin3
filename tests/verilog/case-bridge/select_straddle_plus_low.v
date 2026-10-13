// Task 22 fix round 1: the low end. y[7:4] with s = 3: `y[s +: 2]` covers 3 and 4 and writes y[4].
module t (input [2:0] s, input [1:0] d, input [3:0] a, output reg [7:4] y);
  always @* begin
    y = a;
    y[s +: 2] = d;
  end
endmodule
