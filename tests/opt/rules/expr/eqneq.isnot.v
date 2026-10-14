// expr/eqneq.isnot: 1-bit $eq with constant 0 -> $not
module t(input a, output y);
  wire k = 1'b0;
  assign y = a == k;
endmodule
