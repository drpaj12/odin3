// arst_eq_forms: reset tested with == 1, == 0 and ~
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, r1, r2, r3, input [3:0] d, output reg [3:0] a, b, c);
  always @(posedge clk or posedge r1)
    if (r1 == 1'b1) a <= 4'd1; else a <= d;
  always @(posedge clk or negedge r2)
    if (r2 == 1'b0) b <= 4'd2; else b <= d;
  always @(posedge clk or negedge r3)
    if (~r3) c <= 4'd3; else c <= d;
endmodule
