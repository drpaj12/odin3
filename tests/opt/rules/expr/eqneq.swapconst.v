// expr/eqneq.swapconst: 1-bit $eq with A constant -> A and B swapped
module t(input a, output y);
  assign y = 1'b1 == a;
endmodule
