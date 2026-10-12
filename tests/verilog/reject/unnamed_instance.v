// A module instance without an instance name.
module leaf (input a, output y);
  assign y = a;
endmodule

module m (input a, output y);
  leaf (a, y);
endmodule
