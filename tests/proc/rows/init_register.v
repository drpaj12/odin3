// init_register: initial value on a register
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, input [3:0] d, output reg [3:0] q);
  initial q = 4'd9;
  always @(posedge clk) q <= d;
endmodule
