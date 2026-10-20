module top (  // tests/flatten/keep.v:9
  (* src = "tests/flatten/keep.v:9.13-9.20" *) input x,  // tests/flatten/keep.v:9
  (* src = "tests/flatten/keep.v:9.22-9.29" *) input z,  // tests/flatten/keep.v:9
  (* src = "tests/flatten/keep.v:9.31-9.39" *) output o,  // tests/flatten/keep.v:9
  (* src = "tests/flatten/keep.v:9.41-9.49" *) output p,  // tests/flatten/keep.v:9
  (* src = "tests/flatten/keep.v:9.51-9.59" *) output q  // tests/flatten/keep.v:9
);
  (* src = "tests/flatten/keep.v:6.14-6.21|tests/flatten/keep.v:12.8-12.31" *) wire \e.a ;  // tests/flatten/keep.v:6
  (* src = "tests/flatten/keep.v:6.23-6.30|tests/flatten/keep.v:12.8-12.31" *) wire \e.b ;  // tests/flatten/keep.v:6
  (* src = "tests/flatten/keep.v:6.32-6.40|tests/flatten/keep.v:12.8-12.31" *) wire \e.y ;  // tests/flatten/keep.v:6
  assign \e.a = x;
  assign \e.b = z;
  assign \e.y = q;
  (* src = "tests/flatten/keep.v:10.8-10.31" *)
  kmod k (  // tests/flatten/keep.v:10
    .a(x),
    .b(z),
    .y(o)
  );
  (* keep_hierarchy = 1, src = "tests/flatten/keep.v:11.29-11.53" *)
  leaf ki (  // tests/flatten/keep.v:11
    .a(x),
    .b(z),
    .y(p)
  );
  assign q = x | z;  // tests/flatten/keep.v:7
endmodule
