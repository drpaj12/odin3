// adder, register, adder: the narrowing of one cell exposes the next (several rounds)
module cascade(input clk, input [3:0] a, input [3:0] b, input [3:0] c, output [7:0] y);
  reg [7:0] r;
  wire [7:0] t = {4'b0000, a} + {4'b0000, b};
  always @(posedge clk) r <= t;
  assign y = r + {4'b0000, c};
endmodule
