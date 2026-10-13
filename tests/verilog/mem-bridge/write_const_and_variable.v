module t(input c, we, input [2:0] wa, ra, input [7:0] d, output [7:0] q);
reg [7:0] m [1:4];
always @(posedge c) begin
  if (we) m[wa] <= d;
  m[2] <= 8'h5A;
end
assign q = m[ra];
endmodule
