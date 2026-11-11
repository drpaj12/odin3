// EXPECT: more ordered parameter values than the module has parameters
// DUT: dutparams_dut.v
module dutparam_toomany_tb;
  reg clk = 0;
  reg [7:0] a = 5;
  wire [7:0] q;
  wire [15:0] tag;
  dutparams #(8, 3, 16'h1234, "ab", 7) dut (.clk(clk), .a(a), .q(q), .tag(tag));
  initial begin #1 clk = 1; #1 ("%0d", q); $finish; end
endmodule
