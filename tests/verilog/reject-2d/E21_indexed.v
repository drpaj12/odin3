// E21: an indexed first component that names no block (REJECT here; Yosys makes a wire `u[0].w`).
module leaf (input a, output y);
  wire w = a;
  assign y = w;
endmodule

module m (input a, output y, output z);
  leaf u (.a(a), .y(y));
  assign z = u[0].w;
endmodule
