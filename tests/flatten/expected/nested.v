module top (
  input a,
  input b,
  output y,
  output z,
  output w
);
  wire \ua.i ;
  wire \ua.o ;
  wire \ub.i ;
  wire \ub.o ;
  wire \$c8.p ;
  wire \$c8.q ;
  wire \ua.l1.p ;
  wire \ua.l1.q ;
  wire \ua.l2.p ;
  wire \ua.l2.q ;
  wire \ub.l1.p ;
  wire \ub.l1.q ;
  wire \ub.l2.p ;
  wire \ub.l2.q ;
  wire \$c8.n1 ;
  wire \ua.l1.n1 ;
  wire \ua.l2.n1 ;
  wire \ub.l1.n1 ;
  wire \ub.l2.n1 ;
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
  assign \$c8.n1 = ~a;
  assign w = \$c8.n1 ;
  assign \ua.l1.n1 = ~a;
  assign \ua.l1.q = \ua.l1.n1 ;
  assign \ua.l2.n1 = ~\ua.l1.q ;
  assign y = \ua.l2.n1 ;
  assign \ub.l1.n1 = ~b;
  assign \ub.l1.q = \ub.l1.n1 ;
  assign \ub.l2.n1 = ~\ub.l1.q ;
  assign z = \ub.l2.n1 ;
endmodule
