// M8: macromodule — variants: macromodule with an ANSI header, macromodule with a non-ANSI header
macromodule m8_macro (input a, output y);
  assign y = a;
endmodule

macromodule m8_macro_nonansi (a, y);
  input a;
  output y;
  assign y = a;
endmodule
module m8_top (input a, output y1, y2);
  m8_macro u1 (.a(a), .y(y1));
  m8_macro_nonansi u2 (.a(a), .y(y2));
endmodule
