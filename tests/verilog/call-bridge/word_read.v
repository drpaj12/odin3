// Task 23: a variable index into a net array: a combinational read process, and the same read
// inline in a clocked process.
module t (input clk, input [1:0] s, input [3:0] d, output [3:0] y, output reg [3:0] q);
  wire [3:0] w [0:3];
  assign w[0] = d;
  assign w[1] = ~d;
  assign w[2] = d ^ 4'd9;
  assign w[3] = d + 4'd3;
  assign y = w[s];
  always @(posedge clk) q <= w[s] ^ w[1];
endmodule
