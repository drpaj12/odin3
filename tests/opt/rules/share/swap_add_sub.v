// share/swap_add_sub: d + 1 against 1 - d under one $mux share the 1 on different ports -> one $add on a $neg and a $mux
module t(input s, input [15:0] d, output [15:0] y);
  assign y = s ? d + 16'd1 : 16'd1 - d;
endmodule
