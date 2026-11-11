module memsync_tb;
  reg clk, we;
  reg [2:0] wa, ra;
  reg [7:0] d;
  wire [7:0] q, q2, qa;
  reg [15:0] lfsr;
  integer i;
  xmemsync dut(.clk(clk), .we(we), .wa(wa), .ra(ra), .d(d), .q(q), .q2(q2), .qa(qa));
  always #5 clk = ~clk;
  initial begin
    clk = 0; we = 0; wa = 0; ra = 0; d = 0; lfsr = 16'hace1;
    for (i = 0; i < 300; i = i + 1) begin
      @(negedge clk);
      lfsr = {lfsr[14:0], lfsr[15] ^ lfsr[13] ^ lfsr[12] ^ lfsr[10]};
      we = lfsr[0] | lfsr[5]; wa = lfsr[3:1]; ra = lfsr[8:6]; d = lfsr[15:8] ^ i;
      if (lfsr[11:9] == 0) ra = wa;
      @(posedge clk);
      #1 $display("%0d we=%b wa=%0d ra=%0d d=%h q=%h q2=%h qa=%h", i, we, wa, ra, d, q, q2, qa);
    end
    $finish;
  end
endmodule
