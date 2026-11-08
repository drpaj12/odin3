module poke(input clk, input [2:0] addr, input we, input [7:0] din, output [7:0] data, output [7:0] q);
  reg [7:0] rom [0:7];
  reg [7:0] ram [0:7];
  assign data = rom[addr];
  always @(posedge clk) if (we) ram[addr] <= din;
  assign q = ram[addr];
endmodule
