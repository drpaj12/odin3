// I7: an instance of an undefined module (a black box) — variants: named connections, named connections with a parameter override
// elab-check: --auto-blackbox
module i7_black_box (inout p1, q1, r, p2, q2, s);
  black_box_cell u1 (.a(p1), .b(q1), .y(r));
  black_box_cell #(.W(4)) u2 (.a(p2), .b(q2), .y(s));
endmodule
