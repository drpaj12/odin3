// muxtree/known_inactive: inner mux on the outer A side: its B port is dead
module t(input s, input a, input b, input c, output y);
  assign y = s ? c : (s ? a : b);
endmodule
