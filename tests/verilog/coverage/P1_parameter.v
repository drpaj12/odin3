// P1: parameter in the body — variants: one parameter, a list A = 1, B = 2, a parameter used in an assign
module p1_parameter (output [31:0] y, z);
  parameter W = 8;
  parameter A = 1, B = 2;
  assign y = W;
  assign z = B;
endmodule
