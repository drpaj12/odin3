// A2: a delay on a continuous assignment (IGNORE).
module m (input a, output y);
  assign #1 y = a;
endmodule
