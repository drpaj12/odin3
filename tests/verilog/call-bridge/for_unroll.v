// Task 23: a for loop unrolled; the counter is process-local (no register for it in 2D).
module t (input [3:0] a, output reg [3:0] y);
  integer i;
  always @* begin
    for (i = 0; i < 4; i = i + 1) y[i] = a[3 - i];
  end
endmodule
