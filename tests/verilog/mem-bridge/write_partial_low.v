module t(input c, we, input [2:0] wa, ra, input [3:0] d4, output [7:0] q);
reg [7:0] m [1:4];
always @(posedge c) if (we) m[wa][3:0] <= d4;
assign q = m[ra];
endmodule
