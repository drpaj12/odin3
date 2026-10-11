// Q4: gate delays and strengths (IGNORE) — variants: #1, #(1, 2), a strength pair, a strength pair and a delay, two instances after one delay
module q4_gate_delays (input a, b, output y1, y2, y3, y4, y5, y6);
  and #1 g1 (y1, a, b);
  or #(1, 2) g2 (y2, a, b);
  nand (strong0, weak1) g3 (y3, a, b);
  nor (pull0, pull1) #2 g4 (y4, a, b);
  xor #3 (y5, a, b), (y6, a, b);
endmodule
