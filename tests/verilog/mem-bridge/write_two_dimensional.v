module t(input c, we, input [1:0] wi, wj, ri, rj, input [7:0] d, output [7:0] q);
reg [7:0] m [0:3][0:1];
always @(posedge c) if (we) m[wi][wj[0]] <= d;
assign q = m[ri][rj[0]];
endmodule
