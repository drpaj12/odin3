// N9: memories — variants: reg [7:0] mem [15:0] (descending depth) with a dynamic write in a clocked always and a dynamic read, reg [7:0] rom [0:15] (ascending depth) with a dynamic read
module n9_memory (input clk, we, input [3:0] addr, input [7:0] d, output [7:0] y1, y2);
  reg [7:0] mem [15:0];
  reg [7:0] rom [0:15];
  always @(posedge clk)
    if (we)
      mem[addr] <= d;
  assign y1 = mem[addr];
  assign y2 = rom[addr];
endmodule
