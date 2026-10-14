// expr/xnor_buffer: 1-bit XNOR with constant 1 -> the other input
module t(input a, output y);
  assign y = a ~^ 1'b1;
endmodule
