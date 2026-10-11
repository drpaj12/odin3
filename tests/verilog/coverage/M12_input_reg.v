// M12: input reg and input integer (illegal 2005, accepted by Parmys) — variants: non-ANSI input reg and input integer, ANSI input reg and input integer
module m12_nonansi (a, n, y);
  input reg a;
  input integer n;
  output y;
  assign y = a;
endmodule

module m12_ansi (input reg a, input integer n, output y);
  assign y = a;
endmodule
