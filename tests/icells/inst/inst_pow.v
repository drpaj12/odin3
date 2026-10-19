// 5.2 inst level: one \$pow instance with non-default parameters (read with -icells).
module inst_pow(
  input [3:0] a,
  input [2:0] b,
  output [7:0] y
);
  \$pow #(.A_SIGNED(1), .B_SIGNED(0), .A_WIDTH(4), .B_WIDTH(3), .Y_WIDTH(8)) u (
    .A(a),
    .B(b),
    .Y(y)
  );
endmodule
