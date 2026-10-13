module t(input c, we, input [2:0] wa, wb, ra, input [7:0] d, e, output [7:0] q);
reg [7:0] m [1:4];
always @(posedge c) if (we) m[wa] <= d;
always @(posedge c) if (!we) m[wb] <= e;
assign q = m[ra];
endmodule
