// 5.2 inst level: one \$adffe instance with non-default parameters (read with -icells).
module inst_adffe(
  input clk,
  input arst,
  input en,
  input [3:0] d,
  output [3:0] q
);
  \$adffe #(.WIDTH(4), .CLK_POLARITY(0), .EN_POLARITY(1), .ARST_POLARITY(0), .ARST_VALUE(4'b1010)) u (
    .CLK(clk),
    .ARST(arst),
    .EN(en),
    .D(d),
    .Q(q)
  );
endmodule
