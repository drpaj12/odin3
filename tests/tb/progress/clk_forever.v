// A clock that never stops: no $finish, so only the time limit ends the run.
module t;
  reg clk;
  integer n;
  initial begin clk = 0; n = 0; end
  always #1 begin clk = ~clk; n = n + 1; end
endmodule
