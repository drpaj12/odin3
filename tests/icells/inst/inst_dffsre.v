// 5.2 inst level: one \$dffsre instance with non-default parameters (read with -icells).
module inst_dffsre(
  input clk,
  input [3:0] set,
  input [3:0] clr,
  input en,
  input [3:0] d,
  output [3:0] q
);
  \$dffsre #(.WIDTH(4), .CLK_POLARITY(1), .SET_POLARITY(0), .CLR_POLARITY(1), .EN_POLARITY(0)) u (
    .CLK(clk),
    .SET(set),
    .CLR(clr),
    .EN(en),
    .D(d),
    .Q(q)
  );
endmodule
