// E11: conditional — variants: s ? a : b, a ? b : c ? d : e (right), nested in the true arm s ? t ? a : b : c, s ? a : b + c, s || t ? a : b
module e11_conditional (input s, t, input [3:0] a, b, c, output [3:0] y1, y2, y3, y4, y5);
  assign y1 = s ? a : b;
  assign y2 = s ? a : t ? b : c;
  assign y3 = s ? t ? a : b : c;
  assign y4 = s ? a : b + c;
  assign y5 = s || t ? a : b;
endmodule
