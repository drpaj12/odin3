// expr/fine.cmp: -fine: unsigned X < 0 -> constant 0
// options: -fine
module t(input [3:0] a, output y);
  wire [3:0] z = 4'd0;
  assign y = a < z;
endmodule
