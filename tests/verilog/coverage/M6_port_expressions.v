// M6: port expressions in a module header (REJECT) — variants: .x(a), .w({b, c}), {b, c}, d[1:0]; also a trailing comma (an empty last port), not a REJECT form; `(a, , e)` is a syntax error (Yosys 0.55)
module m6_port_expressions (.x(a), .w({b, c}), {b, c}, d[1:0]);
  input a;
  input b, c;
  input [3:0] d;
endmodule

module m6_empty_ports (a, e, );
  input a;
  input e;
endmodule
