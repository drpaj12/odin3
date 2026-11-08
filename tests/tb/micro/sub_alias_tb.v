// a clock passed through a port reaches the helper in the same delta as the bench (iverilog
// collapses the port into one net)
module sub_alias_tb;
  reg clk;
  wire [3:0] cnt;
  counter u (.clk(clk), .cnt(cnt));
  initial clk = 0;
  always #5 clk = ~clk;
  always @(posedge clk) $display("t=%0t bench sees cnt=%0d", $time, cnt);
  initial #52 $finish;
endmodule
