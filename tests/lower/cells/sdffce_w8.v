module m(input clk, input en, input rst, input [7:0] d, output reg [7:0] q);
  always @(posedge clk) if (en) begin if (rst) q <= 8'hA5; else q <= d; end
endmodule
