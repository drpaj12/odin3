module t(input c, we, input [2:0] wa, ra, input [7:0] d, output reg [7:0] q, output [7:0] w);
  reg [7:0] m [1:4];
  always @(posedge c) begin
    if (we) m[wa] <= d;
    q <= m[ra];
  end
  assign w = m[wa];
endmodule
