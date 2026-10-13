// expr/const_xor: 1-bit XOR of a net with itself -> constant 0
module t(input a, output y);
  assign y = a ^ a;
endmodule
