module t(input c, we, input [2:0] wa, ra, input [7:0] d, output [7:0] q);
reg [7:0] m [1:4];
task wr(input [2:0] a, input [7:0] v);
  m[a] <= v;
endtask
always @(posedge c) if (we) begin wr(wa, d); wr(wa+1, d+1); end
assign q = m[ra];
endmodule
