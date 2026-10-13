// N8: variable declaration initialisers — variants: reg [3:0] r = 4'd5, two scalar regs with initialisers in one declaration, integer n = 7
module n8_var_init (output [3:0] y, output z);
  reg [3:0] r = 4'd5;
  reg s = 1'b1, t = 1'b0;
  integer n = 7;
  assign y = r;
  assign z = s;
endmodule
