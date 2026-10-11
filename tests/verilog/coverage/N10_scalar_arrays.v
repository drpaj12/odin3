// N10: scalar-element arrays — variants: reg a [0:3] without a packed range, two scalar arrays in one declaration with ascending and descending dims, a dynamic read
module n10_scalar_array (input [1:0] i, output y);
  reg a [0:3];
  reg b [3:0], c [0:1];
  assign y = a[i];
endmodule
