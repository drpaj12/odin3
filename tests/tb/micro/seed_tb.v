module seed_tb;
  reg clk;
  wire [3:0] q;
  seed dut(.clk(clk), .q(q));
  initial clk = 1;
  initial begin
    #1 $display("q=%0d", q);
    $finish;
  end
endmodule
