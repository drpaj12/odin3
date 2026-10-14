// muxtree/chain: a three-level tree on one select
module t(input s, input a, input b, input c, input d, output y);
  assign y = s ? (s ? a : b) : (s ? c : d);
endmodule
