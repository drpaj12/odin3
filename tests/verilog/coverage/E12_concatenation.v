// E12: concatenation — variants: {a, b}, a sized literal and a bit-select inside, nested {a, {b, a}}, one element {a}, as an assign target {c, s} (the unsized-operand error is a tests/verilog/reject-2d case)
module e12_concat (input [3:0] a, b, output [7:0] y1, output [8:0] y2, output [11:0] y3, output [3:0] y4, output c, output [3:0] s);
  assign y1 = {a, b};
  assign y2 = {a, 2'b01, b[2:0]};
  assign y3 = {a, {b, a}};
  assign y4 = {a};
  assign {c, s} = a + b;
endmodule
