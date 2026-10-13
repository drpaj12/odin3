// share/and: $and cells sharing an operand under one $mux
module t(input s, input [3:0] a, input [3:0] b, input [3:0] c, output [3:0] y);
  assign y = s ? a & b : a & c;
endmodule
