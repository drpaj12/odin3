// Part A (v1.6.1) micro: an 8 x 16 ROM with a registered read maps to altsyncram ROM; its contents
// go to a .mif (the contents are initial assignments, so the micro needs no data file).
module altsyncram_rom_micro(input clk, input [2:0] addr, output reg [15:0] dout);
  reg [15:0] mem [0:7];
  initial begin
    mem[0] = 16'h0001; mem[1] = 16'h0203; mem[2] = 16'h0405; mem[3] = 16'hf607;
    mem[4] = 16'h0809; mem[5] = 16'h0a0b; mem[6] = 16'h0c0d; mem[7] = 16'h8e0f;
  end
  always @(posedge clk) dout <= mem[addr];
endmodule
