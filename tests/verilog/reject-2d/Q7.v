// Q7: a user-defined primitive (REJECT).
primitive p (y, a);
  output y;
  input a;
  table
    0 : 1;
    1 : 0;
  endtable
endprimitive

module m (input a, output y);
  p g (y, a);
endmodule
