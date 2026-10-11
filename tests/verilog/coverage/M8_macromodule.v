// M8: macromodule — variants: macromodule with an ANSI header, macromodule with a non-ANSI header
macromodule m8_macro (input a, output y);
  assign y = a;
endmodule

macromodule m8_macro_nonansi (a, y);
  input a;
  output y;
  assign y = a;
endmodule
