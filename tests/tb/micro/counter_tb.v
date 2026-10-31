module counter_tb;
  reg clk, rst, en;
  wire [3:0] q;
  wire tc;
  integer n;
  counter dut(.clk(clk), .rst(rst), .en(en), .q(q), .tc(tc));
  initial clk = 0;
  always #5 clk = ~clk;
  initial begin
    rst = 1; en = 0;
    repeat (2) @(posedge clk);
    #1 rst = 0; en = 1;
    for (n = 0; n < 20; n = n + 1) begin
      @(posedge clk);
      #1 $display("t=%0d q=%0d tc=%b", $time, q, tc);
    end
    en = 0;
    repeat (3) @(negedge clk);
    $display("end q=%h", q);
    $finish;
  end
endmodule
