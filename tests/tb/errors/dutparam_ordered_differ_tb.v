// EXPECT: parameter W is given 16 but the DUT was elaborated with 8; pass it to read_verilog (-P, not yet)
// DUT: dutparams_dut.v
module dutparam_ordered_differ_tb;
  reg clk = 0;
  reg [7:0] a = 5;
  wire [7:0] q;
  wire [15:0] tag;
  dutparams #(16) dut (.clk(clk), .a(a), .q(q), .tag(tag));
  initial begin #1 clk = 1; #1 ("%0d", q); $finish; end
endmodule
