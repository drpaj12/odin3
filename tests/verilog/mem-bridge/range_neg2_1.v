module t(input c, we, input signed [2:0] wa, ra, input [7:0] d, output [7:0] q);
reg [7:0] m [-2:1];
always @(posedge c) if (we) m[wa] <= d;
assign q = m[ra];
endmodule
