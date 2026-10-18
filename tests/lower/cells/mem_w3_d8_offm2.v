module m(input clk, input we, input [2:0] wa, input [2:0] wd, input [2:0] ra, output [2:0] rd);
  reg [2:0] mem [-2:5];
  always @(posedge clk) if (we) mem[wa] <= wd;
  assign rd = mem[ra];
endmodule
