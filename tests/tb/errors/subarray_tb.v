// EXPECT: instance array of a testbench module
module subarray_tb;
  wire w;
  leaf u[1:0] (.a(w));
  initial #1 $finish;
endmodule
module leaf(input a);
endmodule
