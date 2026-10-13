module t(input c, we, input [2:0] wa, ra, input [11:0] x, output [7:0] q, output reg [3:0] tt);
reg [7:0] m [1:4];
always @(posedge c) if (we) {tt, m[wa]} <= x;
assign q = m[ra];
endmodule
