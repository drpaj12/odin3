// Task 22: if/else (a switch on s, t1 for y).
module t (input s, a, b, output reg y);
  always @* if (s) y = a; else y = b;
endmodule
