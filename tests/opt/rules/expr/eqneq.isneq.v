// expr/eqneq.isneq: $eq with two different constants at one position -> constant 0
module t(input a, input b, output y);
  assign y = {a, 1'b0} == {b, 1'b1};
endmodule
