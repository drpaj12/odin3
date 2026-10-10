`define ADD(a,b) a+b
`define CALL `ADD
module m(input x, y, output o);
assign o = `CALL(x,y);
endmodule
