`define M a/* c */b
`define N(x) x
module m; wire `M; wire w = `N(p /* q */ + r); wire v = 1 `ifdef Z + 1 `else + 2 `endif ; endmodule
