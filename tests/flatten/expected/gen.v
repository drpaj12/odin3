module top (  // tests/flatten/gen.v:13
  input [2:0] x,  // tests/flatten/gen.v:13
  input [2:0] z,  // tests/flatten/gen.v:13
  output [2:0] o,  // tests/flatten/gen.v:13
  output c  // tests/flatten/gen.v:13
);
  wire [2:0] \m.a ;  // tests/flatten/gen.v:5
  wire [2:0] \m.b ;  // tests/flatten/gen.v:5
  wire [2:0] \m.y ;  // tests/flatten/gen.v:5
  wire \sel.v.a ;  // tests/flatten/gen.v:2
  wire \sel.v.b ;  // tests/flatten/gen.v:2
  wire \sel.v.y ;  // tests/flatten/gen.v:2
  wire \m.g[0].u.a ;  // tests/flatten/gen.v:2
  wire \m.g[0].u.b ;  // tests/flatten/gen.v:2
  wire \m.g[0].u.y ;  // tests/flatten/gen.v:2
  wire \m.g[1].u.a ;  // tests/flatten/gen.v:2
  wire \m.g[1].u.b ;  // tests/flatten/gen.v:2
  wire \m.g[1].u.y ;  // tests/flatten/gen.v:2
  wire \m.g[2].u.a ;  // tests/flatten/gen.v:2
  wire \m.g[2].u.b ;  // tests/flatten/gen.v:2
  wire \m.g[2].u.y ;  // tests/flatten/gen.v:2
  assign \m.a = x;
  assign \m.b = z;
  assign \m.y = o;
  assign \sel.v.a = x[0];
  assign \sel.v.b = z[2];
  assign \sel.v.y = c;
  assign \m.g[0].u.a = x[0];
  assign \m.g[0].u.b = z[0];
  assign \m.g[0].u.y = o[0];
  assign \m.g[1].u.a = x[1];
  assign \m.g[1].u.b = z[1];
  assign \m.g[1].u.y = o[1];
  assign \m.g[2].u.a = x[2];
  assign \m.g[2].u.b = z[2];
  assign \m.g[2].u.y = o[2];
  assign c = x[0] ^ z[2];  // tests/flatten/gen.v:3
  assign o[0] = x[0] ^ z[0];  // tests/flatten/gen.v:3
  assign o[1] = x[1] ^ z[1];  // tests/flatten/gen.v:3
  assign o[2] = x[2] ^ z[2];  // tests/flatten/gen.v:3
endmodule
