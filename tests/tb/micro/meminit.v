// a ROM from an initial block (the INIT parameter) next to a writable memory
module meminit(input clk, input we, input [1:0] wa, input [1:0] ra, input [3:0] d,
               output [3:0] rom_q, output reg [3:0] ram_q);
  reg [3:0] rom [0:3];
  reg [3:0] ram [0:3];
  initial begin
    rom[0] = 4'h3; rom[1] = 4'hc; rom[2] = 4'h5; rom[3] = 4'ha;
    ram[0] = 4'h1; ram[1] = 4'h2; ram[2] = 4'h4; ram[3] = 4'h8;
  end
  assign rom_q = rom[ra];
  always @(posedge clk) begin
    if (we) ram[wa] <= d;
    ram_q <= ram[ra];
  end
endmodule
