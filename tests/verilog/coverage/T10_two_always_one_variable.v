// T10: one variable assigned in two always blocks (REJECT) — variants: two combinational always blocks writing y
module t10_two_always (input a, b, output reg y);
  always @*
    y = a;
  always @*
    y = b;
endmodule
