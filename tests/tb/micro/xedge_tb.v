// The 4-state edge rule of @(posedge) and @(negedge): 0->1, 0->x, x->1 and the mirrors; on a
// multi-bit signal the least significant bit. A DUT flip-flop clocked from an x clock.
module xedge_tb;
  reg s;
  reg [3:0] m;
  reg clk, d;
  wire q;
  integer pe, ne, ch;
  xedge dut(.clk(clk), .d(d), .q(q));
  initial begin pe = 0; ne = 0; ch = 0; end
  always @(posedge s) begin pe = pe + 1; $display("%0t posedge s=%b", $time, s); end
  always @(negedge s) begin ne = ne + 1; $display("%0t negedge s=%b", $time, s); end
  always @(s) begin ch = ch + 1; end
  always @(posedge m) $display("%0t posedge m=%b", $time, m);
  initial begin
    #1 s = 1'b0;
    #1 s = 1'bx;
    #1 s = 1'b1;
    #1 s = 1'bz;
    #1 s = 1'b0;
    #1 s = 1'bz;
    #1 s = 1'bx;
    #1 s = 1'b1;
    #1 s = 1'b0;
    #1 s = 1'b1;
    #1 $display("counts pe=%0d ne=%0d ch=%0d", pe, ne, ch);
    #1 m = 4'b0000;
    #1 m = 4'b1110;
    #1 m = 4'b111x;
    #1 m = 4'b0001;
    #1 $display("q before any clock %b", q);
    d = 1'b1;
    #1 clk = 1'b0;
    #1 $display("after x->0 q=%b", q);
    #1 clk = 1'bx;
    #1 $display("after 0->x q=%b", q);
    d = 1'b0;
    #1 clk = 1'b1;
    #1 $display("after x->1 q=%b", q);
    d = 1'b1;
    #1 clk = 1'b0;
    #1 clk = 1'b1;
    #1 $display("after 0->1 q=%b", q);
    $finish;
  end
endmodule
