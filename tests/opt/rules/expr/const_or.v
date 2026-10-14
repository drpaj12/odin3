// expr/const_or: 1-bit OR with a 1 input -> constant 1
module t(input a, output y);
  assign y = a | 1'b1;
endmodule
