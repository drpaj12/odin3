module t(input c, we, we2, input [2:0] wa, wb, ra, input [7:0] d, e, output [7:0] q);
reg [7:0] m [1:4];
always @(posedge c) begin
  if (we) m[wa] <= d;
  if (we2) m[wb] <= e;
end
assign q = m[ra];
endmodule
