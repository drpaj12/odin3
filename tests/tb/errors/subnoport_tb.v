// EXPECT: module leaf has no port zz
module subnoport_tb;
  wire w;
  leaf u (.a(w), .zz(w));
  initial #1 $finish;
endmodule
module leaf(input a);
endmodule
