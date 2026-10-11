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
