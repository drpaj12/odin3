module t(input c, we, sel, input [2:0] wa, ra, input [7:0] d, e, output [7:0] q);
reg [7:0] m [1:4];
always @(posedge c) begin
  if (we) begin
    if (sel) m[wa] <= d; else m[wa+1] <= e;
  end
end
assign q = m[ra];
endmodule
