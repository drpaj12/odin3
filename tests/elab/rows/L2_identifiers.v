// L2: identifiers with _ and $ — variants: leading _, $ inside, trailing $, digits, upper and lower case distinct (y and Y)
module l2_identifiers (input _start, a$b, output y_1$, Y, y);
  wire mixed_Case$9;
  assign mixed_Case$9 = a$b;
  assign y_1$ = _start;
  assign Y = mixed_Case$9;
  assign y = _start;
endmodule
