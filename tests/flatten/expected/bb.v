module top (  // tests/flatten/bb.v:8
  input x,  // tests/flatten/bb.v:8
  output o,  // tests/flatten/bb.v:8
  output p  // tests/flatten/bb.v:8
);
  wire \m.a ;  // tests/flatten/bb.v:5
  wire \m.y ;  // tests/flatten/bb.v:5
  assign \m.a = x;
  assign \m.y = o;
  bbox t (  // tests/flatten/bb.v:10
    .a(x),
    .y(p)
  );
  bbox \m.b (  // tests/flatten/bb.v:6
    .a(x),
    .y(o)
  );
endmodule
