// N19: a net delay (IGNORE).
module m (input a, output y);
  wire #5 w;
  assign w = a;
  assign y = w;
endmodule
