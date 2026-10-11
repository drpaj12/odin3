// P7: a module-level specparam (IGNORE).
module m (input a, output y);
  specparam p = 1;
  assign y = a;
endmodule
