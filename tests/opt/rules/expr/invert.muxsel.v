// expr/invert.muxsel: $mux whose select is an inverter -> swapped inputs
module t(input s, input a, input b, output y);
  assign y = (~s) ? a : b;
endmodule
