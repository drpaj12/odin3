// E9: case equality — variants: a === b, a !== b, a === 4'b10xz (an x/z operand), a === b == c (left)
module e9_case_equality (input [3:0] a, b, input c, output y1, y2, y3, y4);
  assign y1 = a === b;
  assign y2 = a !== b;
  assign y3 = a === 4'b10xz;
  assign y4 = a === b == c;
endmodule
