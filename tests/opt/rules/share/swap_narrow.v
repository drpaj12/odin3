// share/swap_narrow: signed $add cells with the shared operand on different ports and operands of other widths -> one $add, the narrower operand sign extended
module t(input s, input signed [2:0] a, input signed [1:0] b, input signed [3:0] c, output signed [4:0] y);
  assign y = s ? a + b : c + a;
endmodule
