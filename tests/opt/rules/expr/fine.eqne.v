// expr/fine.eqne: -fine: $eq whose union-find merges 0 with 1 -> constant
// options: -fine
module t(input a, output y);
  assign y = {a, a} == 2'b01;
endmodule
