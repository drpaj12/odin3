module m(input clk, input rst, input d, output reg q);
  always @(posedge clk or posedge rst) if (rst) q <= 1'b1; else q <= d;
endmodule
