// M7: inout ports and tri-state — variants: non-ANSI inout, a ranged inout, inout wire, assign io = oe ? d : 1'bz, an unsized 'bz, a vector tri-state (one top: a tri-state driver is allowed on a top-level port only)
module m7_inout_tristate (io, bus, pin, oe, d, dv, y1, y2);
  inout io;
  inout [3:0] bus;
  inout wire pin;
  input oe, d;
  input [3:0] dv;
  output y1;
  output [3:0] y2;
  assign io = oe ? d : 1'bz;
  assign bus = oe ? dv : 'bz;
  assign pin = oe ? d : 1'bz;
  assign y1 = io;
  assign y2 = bus;
endmodule
