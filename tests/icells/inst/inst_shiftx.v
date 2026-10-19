// 5.2 inst level: one \$shiftx instance with non-default parameters (read with -icells).
module inst_shiftx(
  input [7:0] a,
  input [2:0] b,
  output [3:0] y
);
  \$shiftx #(.A_SIGNED(0), .B_SIGNED(1), .A_WIDTH(8), .B_WIDTH(3), .Y_WIDTH(4)) u (
    .A(a),
    .B(b),
    .Y(y)
  );
endmodule
