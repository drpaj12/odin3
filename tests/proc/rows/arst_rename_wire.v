// arst_rename_wire: reset seen through a renaming wire and an inverter wire
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, rst, rn, input [3:0] d, output reg [3:0] a, b);
  wire r2 = rst;
  wire r3 = ~rn;
  always @(posedge clk or posedge rst)
    if (r2) a <= 4'd0; else a <= d;
  always @(posedge clk or negedge rn)
    if (r3) b <= 4'd9; else b <= d;
endmodule
