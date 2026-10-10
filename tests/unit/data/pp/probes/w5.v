`define CALL `ADD
`define ADD(a,b) a+b
module m(input x, y, output o);
assign o = `CALL
(x,
 y);
endmodule
