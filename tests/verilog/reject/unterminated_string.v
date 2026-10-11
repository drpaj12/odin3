// A string without its closing quote.
module m (output [7:0] y);
  localparam S = "abc;
  assign y = 0;
endmodule
