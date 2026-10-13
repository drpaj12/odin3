// PR 2D-6 review I2: two always blocks write disjoint halves of y through unrolled loops.
module t(input [7:0] a, input [3:0] b, output reg [7:0] y);
  integer i, j;
  always @* for (i = 0; i < 4; i = i + 1) y[i] = a[i];
  always @* for (j = 0; j < 4; j = j + 1) y[4+j] = b[j];
endmodule
