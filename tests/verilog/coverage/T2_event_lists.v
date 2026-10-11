// T2: implicit and listed events — variants: @*, @(*), a comma list @(a, b, c), an or list @(a or b or c), a mixed list @(a or b, c)
module t2_sensitivity (input a, b, c, output reg y1, y2, y3, y4, y5);
  always @*
    y1 = a & b;
  always @(*)
    y2 = a | b;
  always @(a, b, c)
    y3 = a ^ b ^ c;
  always @(a or b or c)
    y4 = a & b | c;
  always @(a or b, c)
    y5 = a | b & c;
endmodule
