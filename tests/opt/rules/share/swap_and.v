// share/swap_and: $and cells sharing a on port A of one and port B of the other (commutative swap) -> one $and behind a $mux
module t(input s, input [3:0] a, input [3:0] b, input [3:0] c, output [3:0] y);
  assign y = s ? a & b : c & a;
endmodule
