module nba_dut_tb;
  reg [3:0] a;
  wire [3:0] y, z;
  assign z = a + 1;
  nba_dut dut(.a(a), .y(y));
  initial begin
    a <= 4'd5;
    #1 $display("%0d %0d", y, z);
    a <= 4'd9;
    #1 $display("%0d %0d", y, z);
    $finish;
  end
endmodule
