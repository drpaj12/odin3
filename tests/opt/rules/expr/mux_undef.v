// expr/mux_undef: $mux with undefined A and B -> Y = A
// options: -mux_undef
module t(input s, output [1:0] y);
  assign y = s ? 2'bxx : 2'bxx;
endmodule
