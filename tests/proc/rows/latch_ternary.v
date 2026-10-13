// latch_ternary: latch whose enable is a ?: net
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input s, a, b, input [3:0] d, output reg [3:0] y);
  wire en = s ? a : b;
  always @* if (en) y = d;
endmodule
