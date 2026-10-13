// vhdl_style: VHDL-style edge test: posedge clk with if (clk)
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, input [3:0] d, output reg [3:0] q);
  always @(posedge clk)
    if (clk) q <= d;
endmodule
