// 3C-2: one 8-bit plus 6-bit unsigned add into 9 bits, mapped by hardmap to one adder chain.
module add_8x6 (
    input  [7:0] a,
    input  [5:0] b,
    output [8:0] y
);
  assign y = a + b;
endmodule
