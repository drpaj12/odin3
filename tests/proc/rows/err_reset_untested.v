// err_reset_untested: second edge event never tested by the body
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, rst, d, output reg q);
  // the body never tests rst
  always @(posedge clk or posedge rst) q <= d;
endmodule
