module mon_tb;
  reg [3:0] a;
  initial begin
    a = 0;
    $monitor("%0d a=%0d", $time, a);
    #5;
    #5 a = 1;
    #5 $finish;
  end
endmodule
