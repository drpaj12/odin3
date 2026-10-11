// L14: undefined macro used — variants: an undefined macro used only inside skipped `ifdef branches (the rejected form, a used undefined macro, is a tests/verilog/reject case)
`ifdef L14_NEVER_DEFINED
`L14_UNDEFINED_MACRO
`endif
module l14_undefined_macro (input a, output y);
`ifdef L14_ALSO_UNDEFINED
  assign y = `L14_NOT_A_MACRO(a);
`else
  assign y = a;
`endif
endmodule
