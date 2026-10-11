// I3: expressions on ports — variants: an expression on an input port (ordered and named), a concatenation on an output port (named and ordered)
module i3_leaf (input [3:0] a, output [3:0] y);
  assign y = ~a;
endmodule

module i3_port_exprs (input [1:0] p, q, input [3:0] r, output [1:0] s, t, v, w);
  i3_leaf u1 (.a(r + 1), .y({s, t}));
  i3_leaf u2 ({p, q}, {v, w});
endmodule
