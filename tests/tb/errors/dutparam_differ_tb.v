// EXPECT: parameter N is given 4 but the DUT was elaborated with 3; read the DUT with 'read_verilog -P N=4'
// DUT: dutparams_dut.v
module dutparam_differ_tb;
  reg clk = 0;
  reg [7:0] a = 5;
  wire [7:0] q;
  wire [15:0] tag;
  dutparams #(.W(8), .N(4)) dut (.clk(clk), .a(a), .q(q), .tag(tag));
  initial begin #1 clk = 1; #1 ("%0d", q); $finish; end
endmodule
