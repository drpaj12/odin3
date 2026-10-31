module ord_b_tb;
  reg clk, rst;
  wire [3:0] q;
  initial clk = 0;
  always #5 clk = ~clk;
  initial begin
    rst = 1;
    #12 rst = 0;
    #40 $finish;
  end
  ord_b dut(.clk(clk), .rst(rst), .q(q));
  always @(posedge clk) $display("B t=%0d q=%0d", $time, q);
endmodule
