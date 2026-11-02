// E21: a hierarchical reference in a parameter override (REJECT).
module leaf (input a, output y);
  wire w = a;
  assign y = w;
endmodule

module sub #(parameter P = 1) (input a, output y);
  assign y = a;
endmodule

module m (input a, output y, output z);
  leaf u (.a(a), .y(y));
  sub #(.P(u.w)) s (.a(a), .y(z));
endmodule
