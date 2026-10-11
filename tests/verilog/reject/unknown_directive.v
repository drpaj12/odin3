// A backtick word that is neither a directive nor a defined macro.
module m (output y);
`frobnicate
  assign y = 0;
endmodule
