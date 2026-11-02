// E21: a hierarchical reference under `default_nettype none (REJECT: no implicit net to make; Yosys also errors).
module leaf (input a, output y);
  wire w = a;
  assign y = w;
endmodule

`default_nettype none
module m (input a, output y, output z);
  leaf u (.a(a), .y(y));
  assign z = u.w;
endmodule
