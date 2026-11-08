// an asynchronous reset asserted and released between clock edges (lower --async keep)
module xasync_tb;
  reg clk, rst, en, srst_n;
  wire [3:0] q, e;
  wire hi;
  xasync u (.clk(clk), .rst(rst), .en(en), .srst_n(srst_n), .q(q), .e(e), .hi(hi));
  initial clk = 0;
  always #5 clk = ~clk;
  initial begin
    rst = 1; en = 1; srst_n = 0;
    #7 rst = 0;
    #1 $display("t=%0t q=%h e=%h", $time, q, e);
    #1 rst = 1;
    #3 $display("t=%0t q=%h e=%h", $time, q, e);
    #20 $display("t=%0t q=%h e=%h hi=%b", $time, q, e, hi);
    en = 0;
    #10 rst = 0; #1 rst = 1;
    #20 $display("t=%0t q=%h e=%h hi=%b", $time, q, e, hi);
    #1 srst_n = 1; #1 srst_n = 0;
    #12 $display("t=%0t q=%h hi=%b", $time, q, hi);
    $finish;
  end
endmodule
