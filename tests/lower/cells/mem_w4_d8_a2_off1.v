module m(input clk, input we, input [1:0] wa, input [3:0] wd, input [1:0] ra, output [3:0] rd);
  reg [3:0] mem [1:8];
  always @(posedge clk) if (we) mem[wa] <= wd;
  assign rd = mem[ra];
endmodule
