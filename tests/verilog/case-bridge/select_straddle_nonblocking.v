// Task 22 fix round 1: the same through a non-blocking write (a flop with a write mux), both ends
// of a [7:4] target: s = 3 writes y[4], s = 7 writes y[7].
module t (input clk, input [2:0] s, input [1:0] d, input [3:0] a, output reg [7:4] y);
  always @(posedge clk) begin
    y <= a;
    y[s +: 2] <= d;
  end
endmodule
