// Hierarchical references into another module (tinyriscv_soc_top.v: u_regs.regs[26] read in an
// always block). As in Yosys each dotted path is an undriven net of the reading module, not
// connected to the other module. The collision: `u.w` is also a wire of the child leaf, which
// flatten renames to `u.w_1` (the foreign net keeps `u.w`; it is undriven and not connected,
// as in Yosys).
module hr_regs (input clk, input d, output q);
  reg [31:0] regs;
  always @(posedge clk) regs <= {regs[30:0], d};
  assign q = regs[0];
endmodule

module hr_leaf (input a, output y);
  wire w;
  assign w = ~a;
  assign y = w;
endmodule

module top (input clk, input d, input a, output reg r, output q, output y, output z);
  hr_regs u_r (.clk(clk), .d(d), .q(q));
  hr_leaf u (.a(a), .y(y));
  always @(posedge clk) r <= u_r.regs[26];
  assign z = u.w;
endmodule
