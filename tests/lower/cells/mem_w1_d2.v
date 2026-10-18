module m(input clk, input we, input wa, input wd, input ra, output rd);
  reg mem [0:1];
  always @(posedge clk) if (we) mem[wa] <= wd;
  assign rd = mem[ra];
endmodule
