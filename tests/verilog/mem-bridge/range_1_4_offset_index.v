module t(input c, we, input [1:0] wa, ra, input [7:0] d, output [7:0] q);
reg [7:0] m [1:4];
wire [2:0] wi = wa + 3'd1;
always @(posedge c) if (we) m[wi] <= d;
assign q = m[ra + 3'd1];
endmodule
