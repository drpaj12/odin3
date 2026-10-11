// The escape \000 would put a NUL byte in a string.
module m (output [7:0] y);
  localparam S = "a\000b";
  assign y = S;
endmodule
