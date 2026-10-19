// 5.2 inst level: one \$dffsr instance with non-default parameters (read with -icells).
module inst_dffsr(
  input clk,
  input [3:0] set,
  input [3:0] clr,
  input [3:0] d,
  output [3:0] q
);
  \$dffsr #(.WIDTH(4), .CLK_POLARITY(0), .SET_POLARITY(1), .CLR_POLARITY(0)) u (
    .CLK(clk),
    .SET(set),
    .CLR(clr),
    .D(d),
    .Q(q)
  );
endmodule
