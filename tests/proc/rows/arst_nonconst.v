// arst_nonconst: asynchronous reset to a non-constant value ($aldff)
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, rst, input [3:0] d, v, output reg [3:0] q);
  always @(posedge clk or posedge rst)
    if (rst) q <= v; else q <= d;
endmodule
