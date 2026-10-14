// expr/eqneq.cmpzero: multi-bit $eq with zero -> $logic_not
module t(input [3:0] a, output y);
  assign y = a == 4'b0000;
endmodule
