// A2: delays on assign (IGNORE) — variants: #1, #(1, 2), #(1, 2, 3), #D with a parameter, #(1:2:3)
module a2_assign_delay (input a, output y1, y2, y3, y4, y5);
  parameter D = 2;
  assign #1 y1 = a;
  assign #(1, 2) y2 = a;
  assign #(1, 2, 3) y3 = a;
  assign #D y4 = a;
  assign #(1:2:3) y5 = a;
endmodule
