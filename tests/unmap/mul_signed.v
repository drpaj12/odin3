// unmap micro: a signed 8x8 multiply into 16 bits (M-2).
module mul_signed (
    input signed [7:0] a,
    input signed [7:0] b,
    output signed [15:0] p
);
  assign p = a * b;
endmodule
