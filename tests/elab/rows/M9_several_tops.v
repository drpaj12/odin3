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
module m9_top (input a, output y1, y2, y3);
  m9_first u1 (.a(a), .y(y1));
  m9_second u2 (.a(a), .y(y2));
  m9_third u3 (.a(a), .y(y3));
endmodule
