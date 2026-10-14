// muxtree/known_active: inner mux under the same select as the outer one: its A port is dead
module t(input s, input a, input b, input c, output y);
  assign y = s ? (s ? a : b) : c;
endmodule
