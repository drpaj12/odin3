// expr/and_or_buffer: 1-bit AND with a 1 input -> the other input
module t(input a, output y);
  assign y = a & 1'b1;
endmodule
