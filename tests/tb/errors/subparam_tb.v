// EXPECT: non-constant
module subparam_tb;
  reg [3:0] r;
  leaf #(r) u ();
  initial #1 $finish;
endmodule
module leaf #(parameter P = 1) ();
endmodule
