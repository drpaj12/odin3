module top (  // tests/flatten/hier.v:9
  (* src = "tests/flatten/hier.v:9.13-9.26" *) input [3:0] x,  // tests/flatten/hier.v:9
  (* src = "tests/flatten/hier.v:9.28-9.41" *) input [3:0] z,  // tests/flatten/hier.v:9
  (* src = "tests/flatten/hier.v:9.43-9.57" *) output [3:0] o,  // tests/flatten/hier.v:9
  (* src = "tests/flatten/hier.v:9.59-9.73" *) output [3:0] p  // tests/flatten/hier.v:9
);
  (* src = "tests/flatten/hier.v:6.13-6.26|tests/flatten/hier.v:10.7-10.31" *) wire [3:0] \m1.a ;  // tests/flatten/hier.v:6
  (* src = "tests/flatten/hier.v:6.28-6.41|tests/flatten/hier.v:10.7-10.31" *) wire [3:0] \m1.b ;  // tests/flatten/hier.v:6
  (* src = "tests/flatten/hier.v:6.43-6.57|tests/flatten/hier.v:10.7-10.31" *) wire [3:0] \m1.y ;  // tests/flatten/hier.v:6
  (* src = "tests/flatten/hier.v:6.13-6.26|tests/flatten/hier.v:11.7-11.31" *) wire [3:0] \m2.a ;  // tests/flatten/hier.v:6
  (* src = "tests/flatten/hier.v:6.28-6.41|tests/flatten/hier.v:11.7-11.31" *) wire [3:0] \m2.b ;  // tests/flatten/hier.v:6
  (* src = "tests/flatten/hier.v:6.43-6.57|tests/flatten/hier.v:11.7-11.31" *) wire [3:0] \m2.y ;  // tests/flatten/hier.v:6
  (* src = "tests/flatten/hier.v:1.31-1.46|tests/flatten/hier.v:7.17-7.40" *) wire [3:0] \m1.u.a ;  // tests/flatten/hier.v:1
  (* src = "tests/flatten/hier.v:1.48-1.63|tests/flatten/hier.v:7.17-7.40" *) wire [3:0] \m1.u.b ;  // tests/flatten/hier.v:1
  (* src = "tests/flatten/hier.v:1.65-1.81|tests/flatten/hier.v:7.17-7.40" *) wire [3:0] \m1.u.y ;  // tests/flatten/hier.v:1
  (* src = "tests/flatten/hier.v:2.15-2.16|tests/flatten/hier.v:7.17-7.40" *) wire [3:0] \m1.u.t ;  // tests/flatten/hier.v:2
  (* src = "tests/flatten/hier.v:1.31-1.46|tests/flatten/hier.v:7.17-7.40" *) wire [3:0] \m2.u.a ;  // tests/flatten/hier.v:1
  (* src = "tests/flatten/hier.v:1.48-1.63|tests/flatten/hier.v:7.17-7.40" *) wire [3:0] \m2.u.b ;  // tests/flatten/hier.v:1
  (* src = "tests/flatten/hier.v:1.65-1.81|tests/flatten/hier.v:7.17-7.40" *) wire [3:0] \m2.u.y ;  // tests/flatten/hier.v:1
  (* src = "tests/flatten/hier.v:2.15-2.16|tests/flatten/hier.v:7.17-7.40" *) wire [3:0] \m2.u.t ;  // tests/flatten/hier.v:2
  assign \m1.a = x;
  assign \m1.b = z;
  assign \m1.y = o;
  assign \m2.a = z;
  assign \m2.b = x;
  assign \m2.y = p;
  assign \m1.u.a = x;
  assign \m1.u.b = z;
  assign \m1.u.y = o;
  assign \m2.u.a = z;
  assign \m2.u.b = x;
  assign \m2.u.y = p;
  assign o = \m1.u.t ^ x;  // tests/flatten/hier.v:4
  assign \m1.u.t = x & z;  // tests/flatten/hier.v:3
  assign p = \m2.u.t ^ z;  // tests/flatten/hier.v:4
  assign \m2.u.t = z & x;  // tests/flatten/hier.v:3
endmodule
