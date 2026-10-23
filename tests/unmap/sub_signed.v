// unmap micro: a signed 8-bit minus signed 4-bit subtraction into 9 bits.
module sub_signed (
    input signed [7:0] a,
    input signed [3:0] b,
    output signed [8:0] y
);
  assign y = a - b;
endmodule
