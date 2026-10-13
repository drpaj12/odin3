// L13: `define — variants: plain macro, macro with arguments, multi-line body with \ continuations, nested use (a macro body using a macro), redefinition
`define L13_WIDTH 4
`define L13_AND(x, y) ((x) & (y))
`define L13_MULTI(x) \
  ((x) | \
   4'b0000)
`define L13_NESTED `L13_AND(a, b)
module l13_define (input [`L13_WIDTH-1:0] a, b, output [`L13_WIDTH-1:0] y1, y2, y3, y4);
  assign y1 = `L13_AND(a, b);
  assign y2 = `L13_MULTI(a);
  assign y3 = `L13_NESTED;
`define L13_WIDTH 8
  assign y4 = `L13_WIDTH;
endmodule
