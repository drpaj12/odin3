`define X 1
`resetall
`ifdef X
wire kept;
`endif
`define P(a) a
wire z = `P
  (3);
