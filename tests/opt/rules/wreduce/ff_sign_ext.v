module ff_sign_ext(input clk, input [3:0] d, output reg [7:0] q);
  always @(posedge clk) q <= {d[3], d[3], d[3], d[3], d};
endmodule
