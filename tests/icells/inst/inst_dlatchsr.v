// 5.2 inst level: one \$dlatchsr instance with non-default parameters (read with -icells).
module inst_dlatchsr(
  input en,
  input [3:0] set,
  input [3:0] clr,
  input [3:0] d,
  output [3:0] q
);
  \$dlatchsr #(.WIDTH(4), .EN_POLARITY(1), .SET_POLARITY(1), .CLR_POLARITY(0)) u (
    .EN(en),
    .SET(set),
    .CLR(clr),
    .D(d),
    .Q(q)
  );
endmodule
