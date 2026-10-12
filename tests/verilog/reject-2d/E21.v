// E21: a hierarchical reference in an expression (REJECT).
module leaf (input a);
  wire w = a;
endmodule

module m (input a, output y);
  leaf u (a);
  assign y = u.w;
endmodule
