// A ROM whose image name is a string parameter and a concatenation of string constants, with a
// zero-padded operand as in {"0"+i, BASE} (NUL bytes drop from the name).
module rmparam #(parameter BASE = "em.hex") (input [2:0] addr, output [7:0] dout);
  localparam [7:0] PFX = 8'd109;  // "m"
  localparam NAME = {PFX, BASE};
  reg [7:0] rom [0:7];
  initial $readmemh(NAME, rom);
  assign dout = rom[addr];
endmodule
