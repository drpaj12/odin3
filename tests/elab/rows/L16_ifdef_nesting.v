// L16: `ifdef `ifndef `else `elsif `endif — variants: taken nesting, `ifdef nested inside a skipped region, `elsif chain, `ifndef with `else
`define L16_A
`define L16_C
module l16_ifdef (input a, b, c, output y, z, w);
`ifdef L16_A
`ifdef L16_C
  assign y = a;
`else
  assign y = b;
`endif
`else
`ifdef L16_C
  assign y = c;
`endif
`endif
`ifdef L16_B
  assign z = a;
`elsif L16_C
  assign z = b;
`else
  assign z = c;
`endif
`ifndef L16_A
  assign w = a;
`else
  assign w = c;
`endif
endmodule
