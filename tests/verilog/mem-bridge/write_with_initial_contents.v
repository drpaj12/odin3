module t(input c, input [1:0] wa, ra, input [7:0] d, output [7:0] q);
reg [7:0] m [0:3];
initial begin m[0] = 8'hAA; m[1] = 8'h55; end
always @(posedge c) m[wa] <= d;
assign q = m[ra];
endmodule
