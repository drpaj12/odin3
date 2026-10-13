module t(input c, we, input [2:0] wa, ra, input [1:0] d2, output [7:0] q);
reg [7:0] m [1:4];
always @(posedge c) if (we) m[wa][5:4] <= d2;
assign q = m[ra];
endmodule
