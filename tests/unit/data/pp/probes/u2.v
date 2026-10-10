`define G(x) x
`define H `G(
module m; wire w = `H 1); endmodule
