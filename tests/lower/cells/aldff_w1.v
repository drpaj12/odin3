module m(input clk, input ld, input ad, input d, output reg q);
  always @(posedge clk or posedge ld) if (ld) q <= ad; else q <= d;
endmodule
