// expr/fine.group: -fine: $and whose bits split into constant and non-constant groups
// options: -fine
module t(input [3:0] a, input [1:0] b, output [3:0] y);
  assign y = a & {b, 2'b11};
endmodule
