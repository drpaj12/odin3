// L21: ignored directives — variants: `celldefine, `endcelldefine, `unconnected_drive pull1 and pull0, `nounconnected_drive, `pragma with and without arguments (`line is consumed by the 2B preprocessor and is covered by its tests)
`celldefine
module l21_cell (input a, output y);
  assign y = a;
endmodule
`endcelldefine
`unconnected_drive pull1
module l21_pull1 (input a, output y);
  assign y = a;
endmodule
`nounconnected_drive
`unconnected_drive pull0
module l21_pull0 (input a, output y);
  assign y = a;
endmodule
`nounconnected_drive
`pragma my_tool option = 1
`pragma reset
module l21_pragma (input a, output y);
  assign y = a;
endmodule
