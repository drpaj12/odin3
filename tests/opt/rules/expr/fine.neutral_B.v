// expr/fine.neutral_B: -fine: $logic_and drops the constant-0 bits of B
// options: -fine
module t(input a, input [1:0] b, output y);
  assign y = a && {b, 2'b00};
endmodule
