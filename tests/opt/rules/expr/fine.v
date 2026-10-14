// expr/fine: -fine: $add whose low result bit is a copy of an operand bit
// options: -fine
module t(input [3:0] a, input [2:0] b, output [3:0] y);
  assign y = a + {b, 1'b0};
endmodule
