// share/shift: $shl cells with the same shifted operand under one $mux
module t(input s, input [3:0] a, input [1:0] b, input [1:0] c, output [3:0] y);
  assign y = s ? a << b : a << c;
endmodule
