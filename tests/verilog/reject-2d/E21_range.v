// E21: a hierarchical reference in a range bound (REJECT).
module leaf (input a, output y);
  wire w = a;
  assign y = w;
endmodule

module m (input a, output y, output [u.w:0] z);
  leaf u (.a(a), .y(y));
endmodule
