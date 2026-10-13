// PR 2D-6 re-review N1: a loop write under a runtime if/case claims only the bits an arm writes.
module t(input clk, input [7:0] a, input [7:0] b, input [3:0] c, input [1:0] s, input e, output reg [7:0] y0, output reg [7:0] y1, output [7:0] q);
reg [7:0] w; assign q = w;
always @* begin : k1 integer i; for (i=0;i<8;i=i+1) if (i==0 || i==2) w[i] = a[i]; end
always @* begin : k2 integer i; for (i=0;i<8;i=i+1) if (i==1 || i==3) if (s[0]) w[i] = b[i]; end
endmodule
