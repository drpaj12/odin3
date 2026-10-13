// err_two_clocks: two edge events of different nets, neither a reset
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, b, d, output reg q);
  // a second edge event that is not an asynchronous reset
  always @(posedge clk or posedge b) q <= d;
endmodule
