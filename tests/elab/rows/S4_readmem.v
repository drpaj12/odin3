// S4: $readmemh and $readmemb in initial — variants: $readmemh with a file name, $readmemb with start and end addresses
module s4_readmem (input [3:0] addr, output [7:0] y, z);
  reg [7:0] rom_h [0:15];
  reg [7:0] rom_b [0:15];
  initial $readmemh("s4_rom.hex", rom_h);
  initial $readmemb("s4_rom.bin", rom_b, 0, 15);
  assign y = rom_h[addr];
  assign z = rom_b[addr];
endmodule
