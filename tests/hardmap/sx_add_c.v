// 3C-2: the combinational twin of sx_add.v (the sim pass has no word-level $dff): two sign-extended
// operands added in an 11-bit context; wreduce leaves a signed 9-bit add into 10 bits.
module sx_add_c (
    input  [8:0]  a,
    input  [8:0]  b,
    output [10:0] y
);
  assign y = {a[8], a[8], a} + {b[8], b[8], b};
endmodule
