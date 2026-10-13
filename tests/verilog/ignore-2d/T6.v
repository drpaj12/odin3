// T6: an always block without an event control (accepted as always @*, with a warning).
module m (output reg clk);
  initial clk = 1'b0;
  always #5 clk = ~clk;
endmodule
