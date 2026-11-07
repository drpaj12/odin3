// A DUT whose registers are x before reset: its outputs before and after reset, with an unknown
// select in an if, a case on an x state and a ?: inside the DUT.
module xdut_tb;
  reg clk, rst;
  reg [1:0] sel;
  reg [7:0] d;
  wire [7:0] q, m, t;
  wire [3:0] st;
  xdut dut(.clk(clk), .rst(rst), .sel(sel), .d(d), .q(q), .m(m), .t(t), .st(st));
  always #10 clk = ~clk;
  initial begin
    #1 clk = 0;
    d = 8'h55;
    #1 $display("t=%0t q=%b m=%b t=%b st=%b", $time, q, m, t, st);
    sel = 2'b01;
    #1 $display("t=%0t q=%b m=%b t=%b st=%b", $time, q, m, t, st);
    sel = 2'b10;
    #1 $display("t=%0t q=%b m=%b t=%b st=%b", $time, q, m, t, st);
    sel = 2'bx0;
    #1 $display("t=%0t q=%b m=%b t=%b st=%b", $time, q, m, t, st);
    @(posedge clk);
    #1 $display("t=%0t q=%b m=%b t=%b st=%b", $time, q, m, t, st);
    rst = 1;
    @(posedge clk);
    #1 $display("t=%0t q=%b m=%b t=%b st=%b", $time, q, m, t, st);
    rst = 0;
    repeat (3) begin
      @(posedge clk);
      #1 $display("t=%0t q=%h m=%h t=%h st=%h", $time, q, m, t, st);
    end
    d = 8'bx;
    @(posedge clk);
    #1 $display("t=%0t q=%h m=%h t=%h st=%h", $time, q, m, t, st);
    $finish;
  end
endmodule
