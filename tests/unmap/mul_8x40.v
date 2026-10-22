// unmap micro: an 8x40 multiply (the other side of the one-side split).
module mul_8x40 (
    input [7:0] a,
    input [39:0] b,
    output [47:0] p
);
  assign p = a * b;
endmodule
