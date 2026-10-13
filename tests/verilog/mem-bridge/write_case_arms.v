module t(input c, input [1:0] s, input [2:0] wa, ra, input [7:0] d, e, output [7:0] q);
reg [7:0] m [1:4];
always @(posedge c) begin
  case (s)
    2'd0: m[wa] <= d;
    2'd1: m[wa] <= e;
    2'd2: ;
    default: m[wa+3'd1] <= d ^ e;
  endcase
end
assign q = m[ra];
endmodule
