// Q3: bufif0/1 and notif0/1 — variants: each of the four, named and unnamed
module q3_tristate_gates (input d, en, output y1, y2, y3, y4);
  bufif0 b0 (y1, d, en);
  bufif1 (y2, d, en);
  notif0 n0 (y3, d, en);
  notif1 (y4, d, en);
endmodule
