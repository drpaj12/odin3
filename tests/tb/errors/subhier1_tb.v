// EXPECT: not an instance of this testbench
module subhier1_tb;
  reg a;
  leaf u (.a(a));
  initial begin nosuch.x = 1; #1 $finish; end
endmodule
module leaf(input a);
  reg x;
endmodule
