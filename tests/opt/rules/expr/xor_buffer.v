// expr/xor_buffer: 1-bit XOR with constant 0 -> the other input
module t(input a, output y);
  assign y = a ^ 1'b0;
endmodule
