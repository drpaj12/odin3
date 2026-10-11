// F1: for loops — variants: in an always with an integer index, counting down past 0 with step -2, a reg index with step 2, in a function
module f1_for (input [7:0] a, output reg [7:0] y1, y2, output [7:0] y3);
  integer i, k;
  reg [3:0] j;
  always @*
    for (i = 0; i < 8; i = i + 1)
      y1[i] = a[7 - i];
  always @* begin
    y2 = 8'd0;
    for (k = 7; k >= 0; k = k - 2)
      y2[k] = a[k];
    for (j = 0; j < 8; j = j + 2)
      y2[j + 1] = a[j];
  end
  function [7:0] rev (input [7:0] x);
    integer n;
    begin
      for (n = 0; n < 8; n = n + 1)
        rev[n] = x[7 - n];
    end
  endfunction
  assign y3 = rev(a);
endmodule
