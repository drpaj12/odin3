// err_dual_edge: both edges of one clock
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, d, output reg q);
  // dual-edge registers are not supported
  always @(posedge clk or negedge clk) q <= d;
endmodule
