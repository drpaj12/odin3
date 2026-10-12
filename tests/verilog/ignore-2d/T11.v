// T11: an intra-assignment delay (IGNORE).
module m (input clk, d, output reg q);
  always @(posedge clk) q <= #1 d;
endmodule
