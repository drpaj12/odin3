// Q4: a gate delay (IGNORE).
module m (input a, b, output y);
  and #2 g1 (y, a, b);
endmodule
