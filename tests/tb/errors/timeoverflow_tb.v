// EXPECT: does not fit in 64 bits
`timescale 1s/1fs
module timeoverflow_tb;
  initial begin
    #20000;
    $display("%t", $time);
    $finish;
  end
endmodule
