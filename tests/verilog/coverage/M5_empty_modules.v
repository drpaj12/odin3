// M5: empty bodies and empty port lists — variants: module m (); with an empty body, module m; with an empty body, a module with ports and no items, each instantiated
module m5_empty_ports ();
endmodule

module m5_no_ports;
endmodule

module m5_no_items (input a);
endmodule

module m5_top (input a);
  m5_empty_ports u1 ();
  m5_no_ports u2 ();
  m5_no_items u3 (a);
endmodule
