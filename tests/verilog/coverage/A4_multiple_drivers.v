// A4: several drivers on one net (REJECT) — variants: two assigns to one wire, a declaration assignment plus an assign
module a4_multi_driver (input a, b, output y, z);
  assign y = a;
  assign y = b;
  wire w = a;
  assign w = b;
  assign z = w;
endmodule
