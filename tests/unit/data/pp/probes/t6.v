`define A ab
`define B cd
`define S(x) "x is x"
`define C(x,y) {x,y}
`define D(x=1, y=2) (x+y)
module m; wire `A`B; wire q = `C({1'b0,1'b1}, 1'b1); initial $display(`S(foo)); wire r = `D(,5); wire s=`D(); endmodule
