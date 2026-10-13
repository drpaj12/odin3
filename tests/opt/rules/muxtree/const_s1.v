// muxtree/const_s1: $mux with a constant-1 select: the A port is dead
module t(input a, input b, input c, output y);
  wire s = 1'b1;
  assign y = s ? (a ? b : c) : c;
endmodule
