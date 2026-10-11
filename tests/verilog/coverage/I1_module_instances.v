// I1: module instances — variants: ordered connections, named connections, an unconnected named port .z(), empty positional slots (p, , r4, )
module i1_leaf (input a, b, output y, z);
  assign y = a & b;
  assign z = a | b;
endmodule

module i1_instances (input p, q, output r1, r2, r3, r4);
  i1_leaf u_ordered (p, q, r1, r2);
  i1_leaf u_named (.a(p), .b(q), .y(r3), .z());
  i1_leaf u_empty (p, , r4, );
endmodule
