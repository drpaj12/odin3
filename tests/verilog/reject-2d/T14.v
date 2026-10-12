// T14: a named event trigger (REJECT).
module m (input clk, output y);
  event e;
  always @(posedge clk) -> e;
  assign y = 1'b0;
endmodule
