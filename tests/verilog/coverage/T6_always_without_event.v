// T6: always without an event control (REJECT) — variants: always #5 clk = ~clk, always begin … end with no timing at all
module t6_no_event (output reg clk, output reg [3:0] n);
  initial clk = 1'b0;
  always #5 clk = ~clk;
  always begin
    n = 4'd0;
  end
endmodule
