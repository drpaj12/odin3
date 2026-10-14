module top (  // tests/flatten/feed.v:9
  input x,  // tests/flatten/feed.v:9
  output o,  // tests/flatten/feed.v:9
  output p,  // tests/flatten/feed.v:9
  output q  // tests/flatten/feed.v:9
);
  wire \t.a ;  // tests/flatten/feed.v:2
  wire \t.y ;  // tests/flatten/feed.v:2
  wire \d.a ;  // tests/flatten/feed.v:5
  wire \d.y ;  // tests/flatten/feed.v:5
  wire \d.k ;  // tests/flatten/feed.v:5
  wire \e.a ;  // tests/flatten/feed.v:5
  wire \e.y ;  // tests/flatten/feed.v:5
  wire \e.k ;  // tests/flatten/feed.v:5
  assign o = x;
  assign q = \d.k ;
  assign \t.a = x;
  assign \t.y = x;
  assign \d.a = x;
  assign \d.y = p;
  assign \e.a = x;
  assign \e.k = \d.k ;
  assign \d.k = 1'b1;
  assign p = ~x;  // tests/flatten/feed.v:6
  assign \e.y = ~x;  // tests/flatten/feed.v:6
endmodule
