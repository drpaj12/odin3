// 5.2 inst level: one \$aldffe instance with non-default parameters (read with -icells).
module inst_aldffe(
  input clk,
  input aload,
  input [3:0] ad,
  input en,
  input [3:0] d,
  output [3:0] q
);
  \$aldffe #(.WIDTH(4), .CLK_POLARITY(0), .EN_POLARITY(1), .ALOAD_POLARITY(1)) u (
    .CLK(clk),
    .ALOAD(aload),
    .AD(ad),
    .EN(en),
    .D(d),
    .Q(q)
  );
endmodule
