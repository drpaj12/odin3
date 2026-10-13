// mem2reg_read: variable-index read of a memory that is also written (mem2reg)
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input c, input [1:0] wa, ra, input [3:0] d, output reg [3:0] q);
  reg [3:0] m [0:3];
  always @(posedge c) begin
    m[wa] <= d;
    q <= m[ra];
  end
endmodule
