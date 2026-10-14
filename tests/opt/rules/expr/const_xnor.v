// expr/const_xnor: 1-bit XNOR of a net with itself -> constant 1
module t(input a, output y);
  assign y = a ~^ a;
endmodule
