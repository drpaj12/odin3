// L20: `resetall — variants: at the start of the file, between modules after a `default_nettype none
`resetall
`default_nettype none
module l20_a (input wire a, output wire y);
  assign y = a;
endmodule
`resetall
module l20_b (input a, output y);
  assign y = a;
endmodule
module l20_top (input a, output y1, y2);
  l20_a u1 (.a(a), .y(y1));
  l20_b u2 (.a(a), .y(y2));
endmodule
