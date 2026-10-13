// L17: `include — variants: an include before the module, a header that includes another header by a path relative to itself (the includer), an include inside a module body
`include "inc/l17_outer.vh"
module l17_include (output y, z, w);
`include "inc/l17_body.vh"
  assign y = `L17_OUTER;
  assign z = `L17_INNER;
  assign w = l17_from_body;
endmodule
