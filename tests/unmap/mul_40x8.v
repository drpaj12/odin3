// unmap micro: a 40x8 multiply, cut by partial_map into two products and one adder (M-4 P1).
module mul_40x8 (
    input [39:0] a,
    input [7:0] b,
    output [47:0] p
);
  assign p = a * b;
endmodule
