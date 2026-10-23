// unmap micro: an 8-bit minus 6-bit unsigned subtraction into 9 bits.
module sub_8x6 (
    input [7:0] a,
    input [5:0] b,
    output [8:0] y
);
  assign y = a - b;
endmodule
