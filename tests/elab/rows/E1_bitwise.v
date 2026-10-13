// E1: bitwise operators — variants: ~a, a & b, a | b, a ^ b, a ~^ b, a ^~ b (one sub-kind), Table 5-4 order a & b | c, a | b & c, a | b ^ c, a ^ b & c; binary a ~& b, a ~| b (not in A.8.6; Yosys and Odin II accept them) read as ~(a & b), ~(a | b) at the levels of & and |, the operator's attributes on the BINARY
module e1_bitwise (input [3:0] a, b, c, output [3:0] y1, y2, y3, y4, y5, y6, y7, y8, y9, y10);
  assign y1 = ~a;
  assign y2 = a & b;
  assign y3 = a | b;
  assign y4 = a ^ b;
  assign y5 = a ~^ b;
  assign y6 = a ^~ b;
  assign y7 = a & b | c;
  assign y8 = a | b & c;
  assign y9 = a | b ^ c;
  assign y10 = a ^ b & c;
endmodule
module e1_binary_nand_nor (input [3:0] a, b, c, output [3:0] y1, y2, y3, y4);
  assign y1 = a ~& b;
  assign y2 = a ~| b;
  assign y3 = a ~& (* k *) b | c;
  assign y4 = a ~| b ^ c;
endmodule
module e1_top (input [3:0] a, b, c, output [3:0] y1, y2, y3, y4, y5, y6, y7, y8, y9, y10, z1, z2, z3, z4);
  e1_bitwise u1 (a, b, c, y1, y2, y3, y4, y5, y6, y7, y8, y9, y10);
  e1_binary_nand_nor u2 (a, b, c, z1, z2, z3, z4);
endmodule
