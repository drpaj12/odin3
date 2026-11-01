// Stand-in for the Altera altpll wrapper `pll_clock` of the Arduino-on-RISC-V design (no source
// is shipped): the core clock is the board clock.
module pll_clock(input areset, input inclk0, output c0);
  assign c0 = inclk0;
endmodule
