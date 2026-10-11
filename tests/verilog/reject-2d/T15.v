// T15: fork ... join (REJECT).
module m (input clk, a, output reg x, z);
  always @(posedge clk)
    fork
      x = a;
      z = a;
    join
endmodule
