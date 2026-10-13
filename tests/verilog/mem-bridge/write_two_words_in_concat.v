module t(input c, we, input [1:0] wa, wb, ra, input [15:0] x, output [7:0] q);
reg [7:0] m [0:3];
always @(posedge c) if (we) {m[wa], m[wb]} <= x;
assign q = m[ra];
endmodule
