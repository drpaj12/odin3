// merge/commutative: a + b and b + a -> one $add
module t(input [3:0] a, input [3:0] b, output [3:0] y, output [3:0] z);
  assign y = a + b;
  assign z = b + a;
endmodule
