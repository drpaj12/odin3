module t(input [3:0] a, output reg [3:0] y, output reg [3:0] z, output reg [3:0] w);
integer i, j;
always @* begin
  z = a;
  for (i = 0; i < 4; i = i + 1) y[i] = a[3 - i];
  for (j = 0; j < 2; j = j + 1) w[j] = a[j];
  w[3:2] = 0;
end
endmodule
