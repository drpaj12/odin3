// Gap 10: at t=100 the clock process (earlier in the source) and the initial block (#100, went to
// sleep first) wake together. iverilog runs the initial block first, so it still sees clk=1 and the
// negedge that follows at the same time; reset is set at t=100, not t=110.
module race_tb;
  reg clk;
  reg reset;
  initial begin clk = 0; reset = 0; end
  always #5 clk = ~clk;
  initial begin
    #100;
    @(negedge clk);
    reset = 1;
    $display("reset=1 at %0t", $time);
    #20;
    $display("done at %0t", $time);
    $finish;
  end
endmodule
