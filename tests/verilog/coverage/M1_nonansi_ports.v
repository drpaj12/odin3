// M1: module…endmodule with a non-ANSI port list — variants: port names in the header, input/output declarations in the body with and without a range, later wire and reg redeclarations of output ports
module m1_nonansi (a, b, y, q, r);
  input a;
  input [3:0] b;
  output y;
  output [3:0] q;
  output r;
  wire y;
  wire [3:0] q;
  reg r;
  assign y = a;
  assign q = b;
endmodule
