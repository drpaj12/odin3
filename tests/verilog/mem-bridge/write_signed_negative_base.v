module t(input c, we, input signed [1:0] wa, ra, input signed [7:0] d, output signed [7:0] q);
reg signed [7:0] m [-2:1];
always @(posedge c) if (we) m[wa] <= d >>> 1;
assign q = m[ra];
endmodule
