// unmap micro: a signed 8-bit plus signed 4-bit add into 9 bits (C-6: signedness from the nets).
module add_signed (
    input signed [7:0] a,
    input signed [3:0] b,
    output signed [8:0] y
);
  assign y = a + b;
endmodule
