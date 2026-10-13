// Task 23: a down-counting integer loop, with a branch on the counter folded per iteration.
module t (input [3:0] a, b, output reg [3:0] y);
  integer i;
  always @* begin
    for (i = 3; i >= 0; i = i - 1)
      if (i == 2) y[i] = a[i] ^ b[i]; else y[i] = a[i] & b[i];
  end
endmodule
