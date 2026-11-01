// A derived clock that never settles: g = clk ^ x ^ y; x toggles on a rising g and y on a falling
// g, so once clk is high g falls after every rise and rises after every fall, forever.
module dosc(input clk, input d, output reg x, output reg y, output reg q);
  initial begin x = 0; y = 0; q = 0; end
  wire g = clk ^ (x ^ y);
  always @(posedge g) x <= ~x;
  always @(negedge g) y <= ~y;
  always @(posedge clk) q <= d;
endmodule
