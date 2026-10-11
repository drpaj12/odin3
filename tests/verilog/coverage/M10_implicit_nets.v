// M10: implicit nets — variants: an undeclared net on the left of a continuous assign, an undeclared net on an instance's output port connection, an undeclared net in an ordered connection
module m10_leaf (input d, output e);
  assign e = d;
endmodule

module m10_implicit (input a, output y, z);
  assign w = a;
  m10_leaf u1 (.d(w), .e(n1));
  m10_leaf u2 (n1, n2);
  assign y = n1;
  assign z = n2;
endmodule
