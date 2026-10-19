// 5.2 inst level: one \$sdffe instance with non-default parameters (read with -icells).
module inst_sdffe(
  input clk,
  input srst,
  input en,
  input [3:0] d,
  output [3:0] q
);
  \$sdffe #(.WIDTH(4), .CLK_POLARITY(1), .EN_POLARITY(0), .SRST_POLARITY(1), .SRST_VALUE(4'b0110)) u (
    .CLK(clk),
    .SRST(srst),
    .EN(en),
    .D(d),
    .Q(q)
  );
endmodule
