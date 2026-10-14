// expr/eqneq.resize: $eq with identical bit positions -> those positions dropped
module t(input a, input b, input c, output y);
  assign y = {a, c} == {b, c};
endmodule
