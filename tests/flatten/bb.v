// A black box instance in a child stays an instance, with the path in its name.
(* blackbox *)
module bbox (input a, output y);
endmodule
module mid (input a, output y);
  bbox b (.a(a), .y(y));
endmodule
module top (input x, output o, output p);
  mid m (.a(x), .y(o));
  bbox t (.a(x), .y(p));
endmodule
