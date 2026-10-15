module top (  // tests/flatten/keep.v:9
  input x,  // tests/flatten/keep.v:9
  input z,  // tests/flatten/keep.v:9
  output o,  // tests/flatten/keep.v:9
  output p,  // tests/flatten/keep.v:9
  output q  // tests/flatten/keep.v:9
);
  wire \e.a ;  // tests/flatten/keep.v:6
  wire \e.b ;  // tests/flatten/keep.v:6
  wire \e.y ;  // tests/flatten/keep.v:6
  assign \e.a = x;
  assign \e.b = z;
  assign \e.y = q;
  kmod k (  // tests/flatten/keep.v:10
    .a(x),
    .b(z),
    .y(o)
  );
  (* keep_hierarchy = 1 *)
  leaf ki (  // tests/flatten/keep.v:11
    .a(x),
    .b(z),
    .y(p)
  );
  assign q = x | z;  // tests/flatten/keep.v:7
endmodule
