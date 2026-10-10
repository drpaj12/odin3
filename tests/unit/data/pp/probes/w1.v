`define DROP(x)
`define KEEP(x) x
`define N(sel) sel(`K)
`define K `N(`DROP)
module m(output o); assign o = 1'b1 `N(`KEEP) ; endmodule
