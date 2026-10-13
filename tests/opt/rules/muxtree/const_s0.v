// muxtree/const_s0: $mux with a constant-0 select: the B port is dead
module t(input a, input b, input c, output y);
  wire s = 1'b0;
  assign y = s ? c : (a ? b : c);
endmodule
