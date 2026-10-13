// The held value (2D spec 8.1, N6): q <= q + 1 reads the register, not the value being assigned.
module t (input clk, output reg [3:0] q);
  always @(posedge clk) q <= q + 1;
endmodule
