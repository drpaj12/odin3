// 3C-2: signed 8-bit minus signed 5-bit into 9 bits; the narrow operand is sign-extended (D1).
module sub_pad_s (
    input  signed [7:0] a,
    input  signed [4:0] b,
    output signed [8:0] y
);
  assign y = a - b;
endmodule
