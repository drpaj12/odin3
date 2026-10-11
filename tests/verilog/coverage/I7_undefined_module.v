// I7: an instance of an undefined module (a black box) — variants: named connections, ordered connections with a parameter override
module i7_black_box (input p, q, output r, s);
  black_box_cell u1 (.a(p), .b(q), .y(r));
  black_box_cell #(4) u2 (p, q, s);
endmodule
