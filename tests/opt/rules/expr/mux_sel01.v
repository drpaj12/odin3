// expr/mux_sel01: $pmux reduced to one select that is also the data -> Y = S
// options: -mux_undef
module t(input s, output y);
  wire [1:0] ss = {1'bx, s};
  assign y = ss[0] ? 1'b1 : 1'b0;
endmodule
