// L6: signed based literals — variants: 4'sb1000, 8'shF0, 4'sd3, 6'so77, upper-case 'S, unsized 'sd5 and 'sh8000_0000
module l6_signed_literals (output signed [3:0] y1, y5, output signed [7:0] y2, y4, output signed [5:0] y3, output signed [31:0] y6, output signed [32:0] y7);
  assign y1 = 4'sb1000;
  assign y2 = 8'shF0;
  assign y3 = 6'so77;
  assign y4 = 8'SB1111_0000;
  assign y5 = 4'sd3;
  assign y6 = 'sd5;
  assign y7 = 'sh8000_0000;
endmodule
