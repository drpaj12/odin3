module t(input c, a, b, input [1:0] s, wa, ra, input [7:0] d, e, output [7:0] q);
reg [7:0] m [0:3];
always @(posedge c) begin
  m[wa] <= d;
  if (a) begin
    if (b) begin
      case (s)
        2'd0: m[wa + 1] <= e;
        2'd1: begin m[wa + 2] <= d; m[wa + 3] <= e; end
        default: ;
      endcase
      m[wa] <= e ^ d;
    end else m[wa + 1] <= d;
    m[wa + 2] <= ~d;
  end
  if (b) m[wa] <= 8'h33;
end
assign q = m[ra];
endmodule
