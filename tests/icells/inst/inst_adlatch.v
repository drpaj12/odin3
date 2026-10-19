// 5.2 inst level: one \$adlatch instance with non-default parameters (read with -icells).
module inst_adlatch(
  input en,
  input arst,
  input [3:0] d,
  output [3:0] q
);
  \$adlatch #(.WIDTH(4), .EN_POLARITY(1), .ARST_POLARITY(0), .ARST_VALUE(4'b0101)) u (
    .EN(en),
    .ARST(arst),
    .D(d),
    .Q(q)
  );
endmodule
