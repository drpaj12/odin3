module top (  // tests/flatten/bb.v:8
  (* src = "tests/flatten/bb.v:8.13-8.20" *) input x,  // tests/flatten/bb.v:8
  (* src = "tests/flatten/bb.v:8.22-8.30" *) output o,  // tests/flatten/bb.v:8
  (* src = "tests/flatten/bb.v:8.32-8.40" *) output p  // tests/flatten/bb.v:8
);
  (* src = "tests/flatten/bb.v:5.13-5.20|tests/flatten/bb.v:9.7-9.23" *) wire \m.a ;  // tests/flatten/bb.v:5
  (* src = "tests/flatten/bb.v:5.22-5.30|tests/flatten/bb.v:9.7-9.23" *) wire \m.y ;  // tests/flatten/bb.v:5
  assign \m.a = x;
  assign \m.y = o;
  (* src = "tests/flatten/bb.v:10.8-10.24" *)
  bbox t (  // tests/flatten/bb.v:10
    .a(x),
    .y(p)
  );
  (* src = "tests/flatten/bb.v:6.8-6.24|tests/flatten/bb.v:9.7-9.23" *)
  bbox \m.b (  // tests/flatten/bb.v:6
    .a(x),
    .y(o)
  );
endmodule
