module pulse_tb;
  reg clk;
  wire [3:0] q;
  pulse dut(.clk(clk), .q(q));
  initial begin
    clk = 0;
    #1 clk = 1; clk = 0;
    #1 $display("a q=%0d", q);
    clk = 1; #0 clk = 0;
    #1 $display("b q=%0d", q);
    #1 clk = 1;
    #1 clk = 0;
    #1 $display("c q=%0d", q);
    clk <= 1;
    #1 $display("d q=%0d", q);
    $finish;
  end
endmodule
