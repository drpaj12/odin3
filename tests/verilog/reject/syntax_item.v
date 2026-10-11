// A syntax error in a module item: a declarator list without its comma.
module m (input a, output y);
  wire b c;
  assign y = a;
endmodule
