module t(input c, input [1:0] s, input [1:0] wa, ra, input [7:0] d, output [7:0] q);
reg [7:0] m [0:3];
always @(posedge c) begin
  (* full_case *) case (s)
    2'd0: m[wa] <= d;
    2'd1: m[wa] <= ~d;
  endcase
end
assign q = m[ra];
endmodule
