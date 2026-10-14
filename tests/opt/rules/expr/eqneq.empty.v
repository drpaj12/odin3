// expr/eqneq.empty: $eq of a net with itself -> constant 1
module t(input [3:0] a, output y);
  assign y = a == a;
endmodule
