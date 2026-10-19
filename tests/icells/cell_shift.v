// 5.2: a variable part select ($shiftx), shifts, and a rotate through a concatenation.
module cell_shift(input [7:0] a, input [2:0] i, input [3:0] s, output [3:0] y, output [7:0] z, output [7:0] w);
  assign y = a[i +: 4];
  assign z = a << s[2:0];
  assign w = $signed(a) >>> s[1:0];
endmodule
