// Part A (v1.6.1) micro: ROMs that stay soft: an asynchronous read, a read enable, and contents
// with unknown bits (word 3 is never written).
module altsyncram_rom_soft_micro(input clk, input ren, input [1:0] addr, output [3:0] q_async,
                                 output reg [3:0] q_en, output reg [3:0] q_x);
  reg [3:0] m1 [0:3];
  reg [3:0] m2 [0:3];
  reg [3:0] m3 [0:3];
  initial begin
    m1[0] = 4'h1; m1[1] = 4'h2; m1[2] = 4'h3; m1[3] = 4'h4;
    m2[0] = 4'h5; m2[1] = 4'h6; m2[2] = 4'h7; m2[3] = 4'h8;
    m3[0] = 4'h9; m3[1] = 4'ha; m3[2] = 4'hb;
  end
  assign q_async = m1[addr];
  always @(posedge clk) if (ren) q_en <= m2[addr];
  always @(posedge clk) q_x <= m3[addr];
endmodule
