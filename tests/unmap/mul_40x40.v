// unmap micro: a 40x40 multiply, cut by partial_map into four products and two adders (M-4 P2).
module mul_40x40 (
    input [39:0] a,
    input [39:0] b,
    output [79:0] p
);
  assign p = a * b;
endmodule
