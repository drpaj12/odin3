// EXPECT: a derived clock of the DUT does not settle
// DUT: ../../sim/dosc.v
module dosc_tb;
  reg clk, d;
  wire x, y, q;
  dosc dut (.clk(clk), .d(d), .x(x), .y(y), .q(q));
  initial begin clk = 0; d = 1; #5 clk = 1; #5 clk = 0; $display("%b%b%b", x, y, q); $finish; end
endmodule
