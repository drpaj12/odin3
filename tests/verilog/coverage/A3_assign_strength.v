// A3: drive strength on assign (IGNORE) — variants: (strong0, weak1), (pull1, supply0), (highz0, strong1), a strength with a delay
module a3_assign_strength (input a, output y1, y2, y3, y4);
  assign (strong0, weak1) y1 = a;
  assign (pull1, supply0) y2 = a;
  assign (highz0, strong1) y3 = a;
  assign (weak0, weak1) #2 y4 = a;
endmodule
