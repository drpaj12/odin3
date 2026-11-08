// EXPECT: call of a task that is not defined
module subhier3_tb;
  reg a;
  leaf u (.a(a));
  initial begin u.nosuch(1); #1 $finish; end
endmodule
module leaf(input a);
  reg x;
endmodule
