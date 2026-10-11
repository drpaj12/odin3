// M9: several top-level modules — variants: three modules in one file, none instantiating another
module m9_first (input a, output y);
  assign y = a;
endmodule

module m9_second (input a, output y);
  assign y = a;
endmodule

module m9_third (input a, output y);
  assign y = a;
endmodule
