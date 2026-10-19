// 5.2 inst level: one \$dlatch instance with non-default parameters (read with -icells).
module inst_dlatch(
  input en,
  input [3:0] d,
  output [3:0] q
);
  \$dlatch #(.WIDTH(4), .EN_POLARITY(0)) u (
    .EN(en),
    .D(d),
    .Q(q)
  );
endmodule
