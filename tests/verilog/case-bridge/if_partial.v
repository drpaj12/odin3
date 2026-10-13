// Task 22: an if without else; --nolatches makes the missing path x on both sides.
module t (input s, a, output reg y);
  always @* if (s) y = a;
endmodule
