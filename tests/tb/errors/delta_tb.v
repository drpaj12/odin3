// EXPECT: time does not advance
module delta_tb;
  reg x;
  initial x = 0;
  initial forever #0 x = ~x;
  initial #10 $finish;
endmodule
