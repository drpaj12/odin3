module decl(a, b, c, y);
	input [3:0] a, b, c;
	output [3:0] y;
	wire [3:0] t = a + b;
	assign y = t & c;
endmodule
