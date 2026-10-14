// expr/const_and: 1-bit AND with a 0 input -> constant 0
module t(input a, output y);
  assign y = a & 1'b0;
endmodule
