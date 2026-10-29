module top (  // tests/flatten/feed.v:9
  (* src = "tests/flatten/feed.v:9.13-9.20" *) input x,  // tests/flatten/feed.v:9
  (* src = "tests/flatten/feed.v:9.22-9.30" *) output o,  // tests/flatten/feed.v:9
  (* src = "tests/flatten/feed.v:9.32-9.40" *) output p,  // tests/flatten/feed.v:9
  (* src = "tests/flatten/feed.v:9.42-9.50" *) output q  // tests/flatten/feed.v:9
);
  (* src = "tests/flatten/feed.v:2.14-2.21|tests/flatten/feed.v:10.8-10.24" *) wire \t.a ;  // tests/flatten/feed.v:2
  (* src = "tests/flatten/feed.v:2.23-2.31|tests/flatten/feed.v:10.8-10.24" *) wire \t.y ;  // tests/flatten/feed.v:2
  (* src = "tests/flatten/feed.v:5.14-5.21|tests/flatten/feed.v:11.8-11.31" *) wire \d.a ;  // tests/flatten/feed.v:5
  (* src = "tests/flatten/feed.v:5.23-5.31|tests/flatten/feed.v:11.8-11.31" *) wire \d.y ;  // tests/flatten/feed.v:5
  (* src = "tests/flatten/feed.v:5.33-5.41|tests/flatten/feed.v:11.8-11.31" *) wire \d.k ;  // tests/flatten/feed.v:5
  (* src = "tests/flatten/feed.v:5.14-5.21|tests/flatten/feed.v:12.8-12.29" *) wire \e.a ;  // tests/flatten/feed.v:5
  (* src = "tests/flatten/feed.v:5.23-5.31|tests/flatten/feed.v:12.8-12.29" *) wire \e.y ;  // tests/flatten/feed.v:5
  (* src = "tests/flatten/feed.v:5.33-5.41|tests/flatten/feed.v:12.8-12.29" *) wire \e.k ;  // tests/flatten/feed.v:5
  assign o = x;
  assign q = \d.k ;
  assign \t.a = x;
  assign \t.y = x;
  assign \d.a = x;
  assign \d.y = p;
  assign \e.a = x;
  assign \e.k = \d.k ;
  assign p = ~x;  // tests/flatten/feed.v:6
  assign \e.y = ~x;  // tests/flatten/feed.v:6
  assign \d.k = 1'b1;  // tests/flatten/feed.v:11
endmodule
