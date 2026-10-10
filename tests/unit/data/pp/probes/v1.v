`define D(x,y) {x y}
`define E(x) [x]
module m; wire a = `D(,1'b1); wire b = `D(1'b0,); wire [1:0] c; wire d = c`E(); endmodule
