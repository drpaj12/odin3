// `default_nettype with a word that is not a net type.
`default_nettype foo
module m (output y);
  assign y = 0;
endmodule
