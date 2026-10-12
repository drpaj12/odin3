// An opaque body without its end keyword.
module m (input a, output y);
  specify
    (a => y) = 1;
endmodule
