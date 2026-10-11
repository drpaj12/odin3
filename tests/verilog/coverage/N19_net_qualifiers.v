// N19: vectored, scalared, strengths and net delays (IGNORE) — variants: vectored, scalared, drive strength with an initialiser, trireg charge strength, wire #5, wire #(1, 2), wire #(1:2:3)
module n19_qualifiers (input a, output y);
  wire vectored [3:0] v;
  wire scalared [3:0] s;
  wire (strong0, weak1) d = a;
  trireg (small) t;
  wire #5 w;
  wire #(1, 2) w2;
  wire #(1:2:3) w3;
  assign y = d;
endmodule
