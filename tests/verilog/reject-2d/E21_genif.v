// E21: a hierarchical reference in a generate-if condition (REJECT).
module leaf (input a, output y);
  wire w = a;
  assign y = w;
endmodule

module m (input a, output y, output z);
  leaf u (.a(a), .y(y));
  generate if (u.w) begin : g
    assign z = a;
  end endgenerate
endmodule
