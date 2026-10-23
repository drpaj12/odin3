// unmap micro: an 80x80 multiply: a split of splits (M-5: the nested pattern needs two scans).
module mul_80x80 (
    input [79:0] a,
    input [79:0] b,
    output [159:0] p
);
  assign p = a * b;
endmodule
