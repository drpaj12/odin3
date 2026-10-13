module t(input c, we, input [1:0] wa, ra, input [7:0] d, output [7:0] q);
reg [7:0] m [0:3];
integer i;
always @(posedge c) begin
  for (i = 0; i < 2; i = i + 1)
    if (we) m[wa + i] <= d + i;
end
assign q = m[ra];
endmodule
