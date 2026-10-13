// L5: unsized literals — variants: unsized based 'hF 'b1 'o17 'd9, unsized decimals 42 and 0 (32-bit signed), more than 32 written bits 'h0_0000_0001, a decimal above 2**31-1
module l5_unsized (output [31:0] y1, y2, y3, y4, y5, y6, output [35:0] y7, output [32:0] y8);
  assign y1 = 'hF;
  assign y2 = 'b1;
  assign y3 = 'o17;
  assign y4 = 'd9;
  assign y5 = 42;
  assign y6 = 0;
  assign y7 = 'h0_0000_0001;
  assign y8 = 2147483648;
endmodule
