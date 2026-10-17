// 3C-1: one 8x7 unsigned multiply, mapped by hardmap to one `multiply` (WIDTH 36).
module mul_8x7 (
    input  [7:0]  a,
    input  [6:0]  b,
    output [14:0] y
);
  assign y = a * b;
endmodule
