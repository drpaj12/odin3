// E21: a hierarchical reference in a constant expression (REJECT).
module leaf (input a, output y);
  wire w = a;
  assign y = w;
endmodule

module m (input a, output y, output z);
  leaf u (.a(a), .y(y));
  localparam L = u.w;
  assign z = a;
endmodule
