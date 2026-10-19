// 5.2 inst level: one \$sr instance with non-default parameters (read with -icells).
module inst_sr(
  input [3:0] set,
  input [3:0] clr,
  output [3:0] q
);
  \$sr #(.WIDTH(4), .SET_POLARITY(0), .CLR_POLARITY(1)) u (
    .SET(set),
    .CLR(clr),
    .Q(q)
  );
endmodule
