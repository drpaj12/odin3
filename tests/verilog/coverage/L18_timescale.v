// L18: `timescale — variants: 1ns / 1ps with blanks, 10ns/1ns without, 100us/10ns between modules
`timescale 1ns / 1ps
module l18_a (input a, output y);
  assign y = a;
endmodule
`timescale 10ns/1ns
module l18_b (input a, output y);
  assign y = a;
endmodule
`timescale 100us/10ns
