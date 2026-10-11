// L7: ? as a z digit — variants: binary 4'b1??0, hex 8'h?F, octal 6'o?7, unsized 'b?, decimal 4'd?, ? mixed with z
module l7_question_digit (output [3:0] y1, y4, y5, output [7:0] y2, output [5:0] y3, output [31:0] y6);
  assign y1 = 4'b1??0;
  assign y2 = 8'h?F;
  assign y3 = 6'o?7;
  assign y4 = 4'd?;
  assign y5 = 4'b?z?Z;
  assign y6 = 'b?;
endmodule
