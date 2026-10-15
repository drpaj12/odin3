// keep_hierarchy on a module and on an instance: both stay as instances.
(* keep_hierarchy *)
module kmod (input a, input b, output y);
  assign y = a & b;
endmodule
module leaf (input a, input b, output y);
  assign y = a | b;
endmodule
module top (input x, input z, output o, output p, output q);
  kmod k (.a(x), .b(z), .y(o));
  (* keep_hierarchy *) leaf ki (.a(x), .b(z), .y(p));
  leaf e (.a(x), .b(z), .y(q));
endmodule
