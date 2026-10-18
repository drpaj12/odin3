module m(input clk, input we, input [2:0] wa, input [1:0] wd, input [2:0] ra, output [1:0] rd);
  reg [1:0] mem [0:5];
  always @(posedge clk) if (we) mem[wa] <= wd;
  assign rd = mem[ra];
endmodule
