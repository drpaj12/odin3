// A DUT instance may repeat the DUT's default parameters: named, ordered, from a testbench
// parameter, with a different width or a string; the result is the one of an override-free instance.
module dutparams_tb;
  localparam WW = 8;
  reg clk = 0;
  reg [7:0] a = 5;
  wire [7:0] q1, q2, q3, q4;
  wire [15:0] t1, t2, t3, t4;
  dutparams #(.W(8), .N(3)) d1 (.clk(clk), .a(a), .q(q1), .tag(t1));
  dutparams #(8, 3, 16'h1234, "ab") d2 (.clk(clk), .a(a), .q(q2), .tag(t2));
  dutparams #(.W(WW), .TAG(4660), .NAME("ab")) d3 (.clk(clk), .a(a), .q(q3), .tag(t3));
  dutparams #(.W(32'd8), .N(1 + 2)) d4 (.clk(clk), .a(a), .q(q4), .tag(t4));
  initial begin
    #1 clk = 1;
    #1 clk = 0;
    $display("%0d %0d %0d %0d", q1, q2, q3, q4);
    $display("%h %h %h %h", t1, t2, t3, t4);
    $finish;
  end
endmodule
