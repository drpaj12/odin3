// E21: a path through a generate block that lacks the member (REJECT here; Yosys makes a wire `g.sub.x`).
module m (input a, output y, output z);
  generate if (1) begin : g
    wire s;
    assign s = a;
  end endgenerate
  assign y = g.s;
  assign z = g.sub.x;
endmodule
