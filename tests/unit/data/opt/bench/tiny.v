module top(input clk, input a, input b, output reg q, output y);
  wire t = a & 1'b0;
  assign y = t | b;
  always @(posedge clk) q <= a ^ b;
endmodule
