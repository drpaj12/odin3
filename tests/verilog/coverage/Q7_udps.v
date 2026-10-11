// Q7: UDPs (REJECT) — variants: a combinational primitive, a sequential primitive with an initial statement, an unnamed and a named UDP instance
primitive q7_mux (y, s, a, b);
  output y;
  input s, a, b;
  table
    0 0 ? : 0;
    0 1 ? : 1;
    1 ? 0 : 0;
    1 ? 1 : 1;
  endtable
endprimitive

primitive q7_dff (q, clk, d);
  output q;
  reg q;
  input clk, d;
  initial q = 1'b0;
  table
    (01) 0 : ? : 0;
    (01) 1 : ? : 1;
    (0?) ? : ? : -;
    (?0) ? : ? : -;
    ? (??) : ? : -;
  endtable
endprimitive

module q7_udp_use (input s, a, b, clk, output y, q);
  q7_mux (y, s, a, b);
  q7_dff u (q, clk, a);
endmodule
