// A non-default DUT parameter override: the DUT is read with `read_verilog -P W=12 -P N=9 -P NAME=xy`
// (dutparamp.rflags) and the testbench gives the same values with #().
module dutparamp_tb;
  reg clk = 0;
  reg [11:0] a = 12'hFFB;
  wire [11:0] q;
  wire [7:0] f;
  dutparamp #(.W(12), .N(9), .NAME("xy")) dut (.clk(clk), .a(a), .q(q), .first(f));
  initial begin
    #1 clk = 1;
    #1 clk = 0;
    $display("%h %c", q, f);
    $finish;
  end
endmodule
