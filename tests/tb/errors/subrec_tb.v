// EXPECT: recursive module instance
module subrec_tb;
  loop u (.a(1'b0));
  initial #1 $finish;
endmodule
module loop(input a);
  loop v (.a(a));
endmodule
