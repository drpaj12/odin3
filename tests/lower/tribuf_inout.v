// A tri-state on inout and output ports of the top (tinyriscv gpio): through proc, opt, lower,
// write_blif and sim. lower drops the enable and drives the port from the data input (WARNING), as
// Parmys does; write_blif and sim then see an inout port driven from inside as an output.
module tribuf_inout (input d, input en, input [1:0] dv, inout io, inout [1:0] bus, output o);
  assign io = en ? d : 1'bz;
  assign bus = en ? dv : 2'bzz;
  assign o = en ? d : 1'bz;
endmodule
