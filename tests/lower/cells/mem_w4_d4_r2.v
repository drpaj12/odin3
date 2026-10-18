module m(input clk, input we, input we2, input [1:0] wa, input [1:0] wa2, input [3:0] wd, input [3:0] wd2, input [1:0] ra, input [1:0] rb, output [3:0] rd, output [3:0] rd2);
  reg [3:0] mem [0:3];
  always @(posedge clk) if (we) mem[wa] <= wd;
  assign rd = mem[ra];
  assign rd2 = mem[rb];
endmodule
