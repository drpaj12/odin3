// expr/eqneq.undef: 1-bit $eq with an undefined constant -> x
module t(input a, output y);
  wire u = 1'bx;
  assign y = a == u;
endmodule
