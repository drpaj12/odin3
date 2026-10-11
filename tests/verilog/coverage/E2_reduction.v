// E2: reduction operators — variants: &a, ~&a, |a, ~|a, ^a, ~^a, ^~a (one sub-kind), ^ over 8 bits, reductions as operands of a binary operator
module e2_reduction (input [3:0] a, input [7:0] w, output y1, y2, y3, y4, y5, y6, y7, y8, y9);
  assign y1 = &a;
  assign y2 = ~&a;
  assign y3 = |a;
  assign y4 = ~|a;
  assign y5 = ^a;
  assign y6 = ~^a;
  assign y7 = ^~a;
  assign y8 = ^w;
  assign y9 = &a | ^w;
endmodule
