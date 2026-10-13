// L19: `default_nettype — variants: none, tri, tri0, tri1, wand, triand, wor, trior, wire (trireg and uwire are legal 2005 values that Icarus 12 rejects, left out so the file stays Icarus-clean)
`default_nettype none
module l19_none (input wire a, output wire y);
  assign y = a;
endmodule
`default_nettype tri
`default_nettype tri0
`default_nettype tri1
`default_nettype wand
`default_nettype triand
`default_nettype wor
`default_nettype trior
`default_nettype wire
module l19_wire (input a, output y);
  assign y = a;
endmodule
module l19_top (input a, output y1, y2);
  l19_none u1 (.a(a), .y(y1));
  l19_wire u2 (.a(a), .y(y2));
endmodule
