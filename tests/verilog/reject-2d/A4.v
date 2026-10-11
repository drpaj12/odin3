// A4: two continuous assignments drive one net (REJECT).
module m (input a, b, output y);
  assign y = a;
  assign y = b;
endmodule
