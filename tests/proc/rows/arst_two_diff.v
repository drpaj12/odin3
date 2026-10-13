// arst_two_diff: two asynchronous resets with different values ($dffsr)
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, r1, r2, input [3:0] d, output reg [3:0] q);
  always @(posedge clk or posedge r1 or negedge r2)
    if (r1) q <= 4'd0;
    else if (!r2) q <= 4'd15;
    else q <= d;
endmodule
