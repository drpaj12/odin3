module top (  // tests/flatten/nested.blif:3
  input a,  // tests/flatten/nested.blif:4
  input b,  // tests/flatten/nested.blif:4
  output y,  // tests/flatten/nested.blif:5
  output z,  // tests/flatten/nested.blif:5
  output w  // tests/flatten/nested.blif:5
);
  wire \ua.i ;  // tests/flatten/nested.blif:14
  wire \ua.o ;  // tests/flatten/nested.blif:15
  wire \ub.i ;  // tests/flatten/nested.blif:14
  wire \ub.o ;  // tests/flatten/nested.blif:15
  wire \$c8.p ;  // tests/flatten/nested.blif:23
  wire \$c8.q ;  // tests/flatten/nested.blif:24
  wire \ua.l1.p ;  // tests/flatten/nested.blif:23
  wire \ua.l1.q ;  // tests/flatten/nested.blif:24
  wire \ua.l2.p ;  // tests/flatten/nested.blif:23
  wire \ua.l2.q ;  // tests/flatten/nested.blif:24
  wire \ub.l1.p ;  // tests/flatten/nested.blif:23
  wire \ub.l1.q ;  // tests/flatten/nested.blif:24
  wire \ub.l2.p ;  // tests/flatten/nested.blif:23
  wire \ub.l2.q ;  // tests/flatten/nested.blif:24
  wire \$c8.n1 ;  // tests/flatten/nested.blif:25
  wire \ua.l1.n1 ;  // tests/flatten/nested.blif:25
  wire \ua.l2.n1 ;  // tests/flatten/nested.blif:25
  wire \ub.l1.n1 ;  // tests/flatten/nested.blif:25
  wire \ub.l2.n1 ;  // tests/flatten/nested.blif:25
  assign \ua.i = a;
  assign \ua.o = y;
  assign \ub.i = b;
  assign \ub.o = z;
  assign \$c8.p = a;
  assign \$c8.q = w;
  assign \ua.l1.p = a;
  assign \ua.l2.p = \ua.l1.q ;
  assign \ua.l2.q = y;
  assign \ub.l1.p = b;
  assign \ub.l2.p = \ub.l1.q ;
  assign \ub.l2.q = z;
  assign \$c8.n1 = ~a;  // tests/flatten/nested.blif:25
  assign w = \$c8.n1 ;  // tests/flatten/nested.blif:28
  assign \ua.l1.n1 = ~a;  // tests/flatten/nested.blif:25
  assign \ua.l1.q = \ua.l1.n1 ;  // tests/flatten/nested.blif:28
  assign \ua.l2.n1 = ~\ua.l1.q ;  // tests/flatten/nested.blif:25
  assign y = \ua.l2.n1 ;  // tests/flatten/nested.blif:28
  assign \ub.l1.n1 = ~b;  // tests/flatten/nested.blif:25
  assign \ub.l1.q = \ub.l1.n1 ;  // tests/flatten/nested.blif:28
  assign \ub.l2.n1 = ~\ub.l1.q ;  // tests/flatten/nested.blif:25
  assign z = \ub.l2.n1 ;  // tests/flatten/nested.blif:28
endmodule
