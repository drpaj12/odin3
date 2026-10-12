// F4: forever (REJECT).
module m (output reg clk);
  initial begin
    clk = 1'b0;
    forever #5 clk = ~clk;
  end
endmodule
