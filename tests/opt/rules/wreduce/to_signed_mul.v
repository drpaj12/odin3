// the product of two sign-extended operands: signed is cheaper
module to_signed_mul(input [3:0] a, input [3:0] b, output [7:0] y);
  assign y = {a[3], a[3], a[3], a[3], a} * {b[3], b[3], b[3], b[3], b};
endmodule
