// N16: supply0 and supply1 — variants: scalar supply0, scalar supply1, a ranged supply1, a list
module n16_supply (output y, z);
  supply0 gnd;
  supply1 vdd;
  supply1 [3:0] vbus;
  supply0 g1, g2;
  assign y = gnd;
  assign z = vdd;
endmodule
