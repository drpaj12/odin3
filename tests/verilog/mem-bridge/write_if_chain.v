module t(input c, a, b, input [1:0] wa, ra, input [7:0] d, e, output [7:0] q);
reg [7:0] m [0:3];
always @(posedge c) begin
  if (a) m[wa] <= d;
  else if (b) m[wa+1] <= e;
  else begin m[wa+2] <= d; m[wa+3] <= e; end
end
assign q = m[ra];
endmodule
