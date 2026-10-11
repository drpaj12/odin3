// T6: an always block without an event control (REJECT).
module m (output reg clk);
  initial clk = 1'b0;
  always #5 clk = ~clk;
endmodule
