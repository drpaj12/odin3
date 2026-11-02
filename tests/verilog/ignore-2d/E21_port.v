// E21: a hierarchical reference in a port connection is accepted (as Yosys): an undriven net u.w
// of the reading module, with a warning; it is not connected to the child's w.
module leaf (input a, output y);
  wire w = a;
  assign y = w;
endmodule

module m (input a, output y, output z);
  leaf u (.a(a), .y(y));
  leaf v (.a(u.w), .y(z));
endmodule
