`timescale 1ns/1ps
// never calls $finish; an event keeps coming (every 4,000,000 ns) past the default limit
module tb_hang;
  reg q;
  initial q = 0;
  always #4000000 q = ~q;
endmodule
