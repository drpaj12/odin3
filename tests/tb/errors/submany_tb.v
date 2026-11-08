// EXPECT: more connections than ports in module leaf
module submany_tb;
  wire w;
  leaf u (w, w);
  initial #1 $finish;
endmodule
module leaf(input a);
endmodule
