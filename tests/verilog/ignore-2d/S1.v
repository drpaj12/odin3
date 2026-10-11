// S1: a specify block (IGNORE).
module m (input a, output y);
  assign y = a;
  specify
    (a => y) = 1;
  endspecify
endmodule
