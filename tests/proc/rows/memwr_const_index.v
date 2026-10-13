// memwr_const_index: memory write at a constant index
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input c, input [2:0] ra, input [7:0] d, output [7:0] q);
  reg [7:0] m [0:7];
  always @(posedge c) m[3] <= d;
  assign q = m[ra];
endmodule
