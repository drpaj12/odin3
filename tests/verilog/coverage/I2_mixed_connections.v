// I2: mixing ordered and named connections (REJECT) — variants: an ordered connection followed by named ones
module i2_leaf (input a, b, output y);
  assign y = a & b;
endmodule

module i2_mixed (input p, q, output r);
  i2_leaf u (p, .b(q), .y(r));
endmodule
