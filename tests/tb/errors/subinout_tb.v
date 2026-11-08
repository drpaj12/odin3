// EXPECT: inout port of a testbench module
module subinout_tb;
  wire w;
  tri u (.p(w));
  initial #1 $finish;
endmodule
module tri(inout p);
endmodule
