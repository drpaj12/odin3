// L15: `undef — variants: `undef of a defined macro, `ifdef after the `undef taking the else branch, `undef of a macro with arguments
`define L15_ON
`define L15_F(x) (x)
`undef L15_ON
`undef L15_F
module l15_undef (input a, output y);
`ifdef L15_ON
  assign y = ~a;
`else
  assign y = a;
`endif
endmodule
