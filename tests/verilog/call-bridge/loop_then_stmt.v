module t(input [3:0] a, output reg [3:0] y, output reg [3:0] z);
integer i;
always @* begin
  for (i = 0; i < 4; i = i + 1) y[i] = a[3 - i];
  z = a;
end
endmodule
