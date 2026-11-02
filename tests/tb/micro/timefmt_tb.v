`timescale 10ns/1ns
module timefmt_tb;
  reg [7:0] r;
  initial begin
    r = 3;
    #5;
    $display("[%t] [%0t] [%10t] [%05t] [%t]", $time, $time, $time, r, 7);
    #12;
    $write("%t|", $time);
    $write("%0t\n", $time);
    $display("%0d %m", $time);
    $finish;
  end
endmodule
