// L8: blanks inside a number — variants: 8 'h FF, 8'h FF, 8 'hFF, 4 'b 1010, 8 'h A5, unsized 'h FF, signed 4 'sd 3 (comments between the pieces are a lexer unit case: Icarus rejects them)
module l8_spaced_numbers (output [7:0] y1, y2, y3, y5, output [3:0] y4, y7, output [31:0] y6);
  assign y1 = 8 'h FF;
  assign y2 = 8'h FF;
  assign y3 = 8 'hFF;
  assign y4 = 4 'b 1010;
  assign y5 = 8 'h A5;
  assign y6 = 'h FF;
  assign y7 = 4 'sd 3;
endmodule
