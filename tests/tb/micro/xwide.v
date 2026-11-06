module xwide(input clk, input [511:0] a, input [511:0] b, output [511:0] s, output reg [511:0] r);
  assign s = a ^ b;
  always @(posedge clk) r <= a + b;
endmodule
