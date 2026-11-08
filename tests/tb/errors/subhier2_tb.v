// EXPECT: not a variable of a testbench-side instance
module subhier2_tb;
  reg a;
  leaf u (.a(a));
  initial begin $display("%d", u.nosuch); #1 $finish; end
endmodule
module leaf(input a);
  reg x;
endmodule
