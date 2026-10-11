// C1: if / else if / else — variants: if alone, if-else, an else-if chain, a nested if with its else bound to the inner if
module c1_if_else (input a, b, c, input [1:0] s, output reg y1, y2, y3, y4);
  always @* begin
    y1 = 1'b0;
    if (a)
      y1 = b;
  end
  always @*
    if (a)
      y2 = b;
    else
      y2 = c;
  always @*
    if (s == 2'd0)
      y3 = a;
    else if (s == 2'd1)
      y3 = b;
    else
      y3 = c;
  always @* begin
    y4 = 1'b0;
    if (a)
      if (b)
        y4 = c;
      else
        y4 = 1'b1;
  end
endmodule
