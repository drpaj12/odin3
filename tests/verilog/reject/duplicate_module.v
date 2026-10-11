// M11: a module defined twice in one file.
module m (output y);
  assign y = 0;
endmodule

module m (output y);
  assign y = 1;
endmodule
