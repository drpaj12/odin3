// 5.2 inst level: one \$aldff instance with non-default parameters (read with -icells).
module inst_aldff(
  input clk,
  input aload,
  input [3:0] ad,
  input [3:0] d,
  output [3:0] q
);
  \$aldff #(.WIDTH(4), .CLK_POLARITY(1), .ALOAD_POLARITY(0)) u (
    .CLK(clk),
    .ALOAD(aload),
    .AD(ad),
    .D(d),
    .Q(q)
  );
endmodule
