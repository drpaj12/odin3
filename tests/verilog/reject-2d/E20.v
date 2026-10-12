// E20: a system function in an expression (REJECT).
module m (output [31:0] y);
  assign y = $random;
endmodule
