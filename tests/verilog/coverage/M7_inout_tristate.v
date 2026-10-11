// M7: inout ports and tri-state — variants: non-ANSI inout, ANSI inout with a range, inout wire, assign io = oe ? d : 1'bz, an unsized 'bz, a vector tri-state
module m7_nonansi (io, oe, d, y);
  inout io;
  input oe, d;
  output y;
  assign io = oe ? d : 1'bz;
  assign y = io;
endmodule

module m7_ansi (inout [3:0] bus, inout wire pin, input oe, input [3:0] d, output [3:0] y);
  assign bus = oe ? d : 'bz;
  assign pin = oe ? d[0] : 1'bz;
  assign y = bus;
endmodule
