module m(input clk, input en, input rst, input d, output reg q);
  always @(posedge clk) if (en) begin if (rst) q <= 1'b1; else q <= d; end
endmodule
