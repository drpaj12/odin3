// I2: an instance mixing ordered and named connections (REJECT).
module leaf (input a, b, output y);
  assign y = a & b;
endmodule

module m (input p, q, output r);
  leaf u (p, .b(q), .y(r));
endmodule
