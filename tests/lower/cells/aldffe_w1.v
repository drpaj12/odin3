module m(input clk, input en, input ld, input ad, input d, output reg q);
  always @(posedge clk or posedge ld) if (ld) q <= ad; else if (en) q <= d;
endmodule
