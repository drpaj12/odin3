// expr/xbit: $add with an x operand -> all-x result
module t(input [3:0] a, output [3:0] y);
  assign y = a + 4'bxxxx;
endmodule
