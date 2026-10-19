// share/chunk: $and cells that drive the low half of a wider $mux port -> one $and behind a $mux, the high half stays
module t(input s, input [3:0] a, input [3:0] b, input [3:0] c, input [3:0] d, output [7:0] y);
  assign y = s ? {d, a & b} : {d, a & c};
endmodule
