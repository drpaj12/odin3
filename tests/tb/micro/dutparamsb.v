module dutparamsb(a, q);
  parameter W = 4;
  parameter integer N = -1;
  input [W-1:0] a; output [W-1:0] q;
  assign q = a;
endmodule
