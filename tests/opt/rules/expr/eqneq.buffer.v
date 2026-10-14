// expr/eqneq.buffer: 1-bit $eq with constant 1 -> Y = A
module t(input a, output y);
  wire k = 1'b1;
  assign y = a == k;
endmodule
