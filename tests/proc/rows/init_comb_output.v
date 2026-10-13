// init_comb_output: initial value on a combinational output
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input s, input [3:0] a, b, output reg [3:0] y);
  initial y = 4'd3;
  always @* if (s) y = a; else y = b;
endmodule
