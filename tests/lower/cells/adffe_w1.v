module m(input clk, input en, input rst, input d, output reg q);
  always @(posedge clk or posedge rst) if (rst) q <= 1'b1; else if (en) q <= d;
endmodule
