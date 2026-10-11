// P7: specparam (IGNORE) — variants: module-level specparam, a module-level list, a specparam list inside specify
module p7_specparam (input a, output y);
  specparam tRise = 2;
  specparam tFall = 3, tHold = 1;
  assign y = a;
  specify
    specparam tA = 1, tB = 2;
    (a => y) = (tA, tB);
  endspecify
endmodule
