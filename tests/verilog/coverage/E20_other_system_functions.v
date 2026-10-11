// E20: other system functions in expressions (REJECT) — variants: $random without arguments, $time, $rtoi(2.5)
module e20_system_functions (output [31:0] y1, output [63:0] y2, output [31:0] y3);
  assign y1 = $random;
  assign y2 = $time;
  assign y3 = $rtoi(2.5);
endmodule
