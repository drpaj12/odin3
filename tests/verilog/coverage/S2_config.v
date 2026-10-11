// S2: config…endconfig (REJECT) — variants: design, default liblist, instance use, cell use
module s2_top (input a, output y);
  assign y = a;
endmodule

config s2_cfg;
  design work.s2_top;
  default liblist work;
  instance s2_top use work.s2_top;
  cell s2_top use work.s2_top;
endconfig
