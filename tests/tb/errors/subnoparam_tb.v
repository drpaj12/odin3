// EXPECT: module leaf has no parameter Q
module subnoparam_tb;
  leaf #(.Q(2)) u ();
  initial #1 $finish;
endmodule
module leaf #(parameter P = 1) ();
endmodule
