// clean/dead_chain: a chain of cells feeding nothing is removed
module t(input a, input b, output y);
  wire t = ~a;
  wire u = ~t;
  assign y = a | b;
endmodule
