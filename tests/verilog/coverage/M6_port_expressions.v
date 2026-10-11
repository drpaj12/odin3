// M6: port expressions in a module header (REJECT) — variants: .x(a), .w({b, c}), {b, c}, d[1:0]; also empty ports (a, , e) and a trailing comma, which are not REJECT forms
module m6_port_expressions (.x(a), .w({b, c}), {b, c}, d[1:0]);
  input a;
  input b, c;
  input [3:0] d;
endmodule

module m6_empty_ports (a, , e, );
  input a;
  input e;
endmodule
