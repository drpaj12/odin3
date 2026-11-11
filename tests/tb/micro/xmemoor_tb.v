module memoor_tb;
  reg clk, we, lo;
  reg [2:0] wa, ra;
  reg [7:0] d;
  wire [7:0] q, qa;
  reg [15:0] lfsr;
  integer i;
  xmemoor dut(.clk(clk), .we(we), .lo(lo), .wa(wa), .ra(ra), .d(d), .qa(qa), .q(q));
  always #5 clk = ~clk;
  initial begin
    clk = 0; we = 0; lo = 0; wa = 0; ra = 0; d = 0; lfsr = 16'h1234;
    for (i = 0; i < 300; i = i + 1) begin
      @(negedge clk);
      lfsr = {lfsr[14:0], lfsr[15] ^ lfsr[13] ^ lfsr[12] ^ lfsr[10]};
      we = lfsr[0] & lfsr[4]; lo = lfsr[5] & ~lfsr[6]; wa = lfsr[3:1]; ra = lfsr[8:6]; d = lfsr[15:8] ^ i;
      #1 $display("%0d ra=%0d qa=%b", i, ra, qa);
      @(posedge clk);
      #1 $display("%0d we=%b lo=%b wa=%0d ra=%0d d=%h q=%b qa=%b", i, we, lo, wa, ra, d, q, qa);
    end
    ra = 3'bx; #1 $display("xread qa=%b", qa);
    ra = 3'b1x0; #1 $display("xread2 qa=%b", qa);
    $finish;
  end
endmodule
