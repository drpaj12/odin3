// arst_negedge_not: asynchronous reset, negedge, tested with !
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, rst_n, input [3:0] d, output reg [3:0] q);
  always @(negedge clk or negedge rst_n)
    if (!rst_n) q <= 4'd5; else q <= d;
endmodule
