// pp-diff fixture (2B-3 final review): comments in macro formal defaults. Icarus keeps the
// comment text in the expansion, Yosys and Odin III drop it; the tokens are identical, so the
// oracles agree and Odin III must match them (PHASE2 #12, M7 list).
`define A(x=/*c*/ 3) [x]
`define W(hi=/* msb */ 7, lo = 0 /* lsb */) [hi:lo]
module m;
    wire `A() a;
    wire `W() w;
    wire `W(3) v;
    wire `W(, 2) u;
endmodule
